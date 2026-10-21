/**
 * @file connection.cppm
 * @brief Own strand-serialized TCP transfers, bounded buffers, deadlines, and connection shutdown.
 */
module mhttp:core.impl.connection;

import std;
import asio;
import :core.error;
import :core.impl.io_error;

namespace mhttp::impl {

    /**
     * @brief Bound memory used by an individual connection transfer.
     */
    struct ConnectionLimits {
        static constexpr std::size_t MAX_READ_BUFFER_SIZE{ 64 * 1024 }; ///< Maximum receive scratch space.
        std::size_t read_buffer_size{ 16 * 1024 }; ///< Bytes received by one ReadSome, positive and bounded.
        std::size_t max_write_size{ 16 * 1024 * 1024 }; ///< Maximum owned payload of one WriteAll, positive.
    };

    /**
     * @brief Preserve the transport-reported completed prefix even when a whole write fails.
     * @note A successful result counts the entire payload. The count describes local operation
     *       completion, not peer acknowledgment; it cannot prove remote application consumption.
     */
    struct WriteOutcome {
        std::size_t bytes_transferred{}; ///< Completed bytes reported by the composed write operation.
        std::optional<Error> error; ///< Absent only when the complete owned payload was written.
    };

    /**
     * @brief Own a TCP socket and serialize every transfer, deadline, and close on one strand.
     * @note Create through Create and retain shared ownership. At most one read and one write may
     *       be pending; overlapping operations in the same direction return Busy. Successful reads
     *       own their bytes independently of subsequent reads. Runtime must outlive this connection.
     */
    class Connection : public std::enable_shared_from_this<Connection> {
    private:
        using Duration = std::chrono::steady_clock::duration; ///< Relative deadline type.

        /**
         * @brief Keep a canceled timer callback from acting on a later operation.
         */
        struct Deadline {
            asio::steady_timer timer; ///< Timer owned through its completion callback.
            bool finished{}; ///< Transfer already finished; a queued timer callback is harmless.
            bool expired{}; ///< Timer won and closed this connection.

            /**
             * @brief Bind timer state to the connection strand.
             * @param executor Connection's serialized executor.
             */
            explicit Deadline(asio::any_io_executor executor) : timer{ executor } {}
        };

        /**
         * @brief Release a direction's in-flight flag and retire its timer on every exit path.
         */
        struct ActiveOperation {
            bool& busy; ///< Direction state borrowed from the coroutine-owned connection.
            std::shared_ptr<Deadline> deadline; ///< State retained by any queued timer completion.

            /**
             * @brief Mark a direction active after its deadline allocation succeeds.
             * @param active In-flight flag, accessed only on the connection strand.
             * @param state Owned timer state.
             */
            ActiveOperation(bool& active, std::shared_ptr<Deadline> state) :
                busy{ active }, deadline{ std::move(state) } { busy = true; }

            /**
             * @brief Keep direction and timer cleanup unique to one coroutine scope.
             * @param other Unused source.
             */
            ActiveOperation(ActiveOperation const& other) = delete;

            /**
             * @brief Retire callbacks before another transfer can reuse this direction.
             */
            ~ActiveOperation() {
                busy = false;
                deadline->finished = true;
                static_cast<void>(deadline->timer.cancel());
            }
        };

        asio::any_io_executor m_executor; ///< Strand owning every mutable field below.
        asio::ip::tcp::socket m_socket; ///< Socket whose executor is the connection strand.
        std::vector<char> m_read_buffer; ///< Fixed receive scratch space, never grown after construction.
        std::size_t m_max_write_size; ///< Limit validated before any payload is sent.
        bool m_reading{}; ///< One outstanding ReadSome at most.
        bool m_writing{}; ///< One outstanding WriteAll at most.
        bool m_read_eof{}; ///< Peer half-closed its send direction; writes may still succeed.
        bool m_send_shutdown{}; ///< Local send direction already half-closed.
        bool m_closed{}; ///< Local close requested and applied on this strand.

        /**
         * @brief Construct with validated limits before any socket operation is started.
         * @param executor Runtime executor; a fresh connection strand is created.
         * @param limits Validated positive transfer limits.
         */
        Connection(asio::any_io_executor executor, ConnectionLimits limits) :
            m_executor{ asio::make_strand(executor) }, m_socket{ m_executor },
            m_read_buffer(limits.read_buffer_size), m_max_write_size{ limits.max_write_size } {}

    public:
        /**
         * @brief Create shared connection ownership without allocating an invalid buffer size.
         * @param executor Executor borrowed from the longer-lived runtime.
         * @param limits Receive scratch size and maximum single write payload.
         * @return New connection, or InvalidRequest for invalid limits.
         */
        [[nodiscard]] static auto Create(
            asio::any_io_executor executor,
            ConnectionLimits limits = {}
        ) -> Result<std::shared_ptr<Connection>> {
            if (limits.read_buffer_size == 0 || limits.read_buffer_size > ConnectionLimits::MAX_READ_BUFFER_SIZE
                || limits.max_write_size == 0) {
                return std::unexpected{ Error::InvalidRequest };
            }
            return std::shared_ptr<Connection>{ new Connection{ std::move(executor), limits } };
        }

        /**
         * @brief Borrow the unopened socket solely for accept/connect initialization and options.
         * @return Socket bound to this connection's strand.
         * @note Retain this connection until initialization finishes. Do not access this reference
         *       concurrently or after transferring control to ReadSome, WriteAll, Shutdown, or Close.
         */
        [[nodiscard]] auto SocketForInitialization() noexcept -> asio::ip::tcp::socket& { return m_socket; }

        /**
         * @brief Borrow the serialized executor for session roots and transport bridges.
         * @return Owned executor copy; the runtime must remain alive.
         */
        [[nodiscard]] auto Executor() const -> asio::any_io_executor { return m_executor; }

        /**
         * @brief Inspect the immutable single-write byte bound before a session starts.
         * @return Maximum owned payload accepted by WriteAll.
         */
        [[nodiscard]] auto MaxWriteSize() const noexcept -> std::size_t { return m_max_write_size; }

        /**
         * @brief Receive one bounded owned chunk on the connection strand.
         * @param timeout Relative deadline; nullopt disables it, nonpositive durations expire immediately.
         * @return Owned bytes, Busy, ConnectionClosed, Canceled, ReadTimeout, or a read failure.
         * @note The coroutine owns this connection before initial suspension.
         */
        [[nodiscard]] auto ReadSome(
            std::optional<Duration> timeout = std::chrono::seconds{ 30 }
        ) -> asio::awaitable<Result<std::string>> {
            return asio::co_spawn(m_executor, readSome(shared_from_this(), timeout), asio::use_awaitable);
        }

        /**
         * @brief Send every byte of an owned payload with one deadline for the whole transfer.
         * @param bytes Payload moved into the operation, retained through partial writes and cancellation.
         * @param timeout Relative deadline; nullopt disables it, nonpositive durations expire immediately.
         * @return Success, Busy, BodyTooLarge before any send, or a transport/timeout error.
         * @note A failed or timed-out write closes the connection because a prefix may have been sent.
         */
        [[nodiscard]] auto WriteAll(
            std::string bytes,
            std::optional<Duration> timeout = std::chrono::seconds{ 30 }
        ) -> asio::awaitable<Result<void>> {
            return asio::co_spawn(m_executor, writeAll(shared_from_this(), std::move(bytes), timeout), asio::use_awaitable);
        }

        /**
         * @brief Send an owned payload and retain completed-prefix information on every exit path.
         * @param bytes Owned payload, kept alive through partial writes and cancellation.
         * @param timeout One relative deadline for the whole write; nullopt disables it.
         * @return Complete byte count and optional failure; admission/closed/immediate failures count zero.
         * @note Shares WriteAll's in-flight guard and close policy. A positive count always forbids
         *       transparent request replay; a zero count alone does not make cancellation/timeout retryable.
         */
        [[nodiscard]] auto WriteAllWithProgress(
            std::string bytes,
            std::optional<Duration> timeout = std::chrono::seconds{ 30 }
        ) -> asio::awaitable<WriteOutcome> {
            return asio::co_spawn(m_executor, writeWithProgress(shared_from_this(), std::move(bytes), timeout), asio::use_awaitable);
        }

        /**
         * @brief Half-close sending after completed writes, while allowing remaining peer bytes to be read.
         * @return Success if already shut down/closed, Busy during a write, or a shutdown error.
         */
        [[nodiscard]] auto Shutdown() -> asio::awaitable<Result<void>> {
            return asio::co_spawn(m_executor, shutdown(shared_from_this()), asio::use_awaitable);
        }

        /**
         * @brief Request idempotent close from any thread, posting socket access to the owning strand.
         * @note Pending operations drain as Canceled; future operations return ConnectionClosed.
         *       Keep the runtime executing until the queued close and canceled completions drain.
         */
        auto Close() -> void {
            asio::post(m_executor, [self = shared_from_this()] { self->close(); });
        }

    private:
        /**
         * @brief Arm one operation deadline, retaining connection ownership until timer completion.
         * @param deadline Operation state, accessed only on this strand.
         * @param timeout Optional relative deadline.
         */
        auto armDeadline(
            std::shared_ptr<Deadline> deadline,
            std::optional<Duration> timeout
        ) -> void {
            if (!timeout) { return; }
            if (*timeout <= Duration::zero()) {
                deadline->expired = true;
                close();
                return;
            }
            deadline->timer.expires_after(*timeout);
            deadline->timer.async_wait([self = shared_from_this(), deadline](asio::error_code error) {
                if (!error && !deadline->finished) {
                    deadline->expired = true;
                    self->close();
                }
            });
        }

        /**
         * @brief Apply close solely on the connection strand, preserving pending operation lifetimes.
         */
        auto close() noexcept -> void {
            if (m_closed) { return; }
            m_closed = true;
            asio::error_code ignored;
            m_socket.shutdown(asio::socket_base::shutdown_send, ignored);
            m_socket.close(ignored);
        }

        /**
         * @brief Receive while retaining the connection independently of the caller's owner.
         * @param self Owned connection for the complete coroutine lifetime.
         * @param timeout Optional operation deadline.
         * @return One owned chunk or a contextual failure.
         */
        static auto readSome(
            std::shared_ptr<Connection> self,
            std::optional<Duration> timeout
        ) -> asio::awaitable<Result<std::string>> {
            co_await asio::this_coro::throw_if_cancelled(false);
            if (self->m_closed || self->m_read_eof) { co_return std::unexpected{ Error::ConnectionClosed }; }
            if (self->m_reading) { co_return std::unexpected{ Error::Busy }; }
            auto cancellation = co_await asio::this_coro::cancellation_state;
            if (cancellation.cancelled() != asio::cancellation_type::none) {
                self->close();
                co_return std::unexpected{ Error::Canceled };
            }
            auto deadline = std::make_shared<Deadline>(self->m_executor);
            ActiveOperation active{ self->m_reading, deadline };
            self->armDeadline(deadline, timeout);
            if (deadline->expired) { co_return std::unexpected{ Error::ReadTimeout }; }
            auto const [error, count] = co_await self->m_socket.async_read_some(
                asio::buffer(self->m_read_buffer), asio::as_tuple(asio::use_awaitable));
            if (deadline->expired) { co_return std::unexpected{ Error::ReadTimeout }; }
            if (self->m_closed) { co_return std::unexpected{ Error::Canceled }; }
            if (auto failure = map_io_error(error, IoOperation::Read)) {
                if (error == asio::error::eof) { self->m_read_eof = true; }
                else { self->close(); }
                co_return std::unexpected{ *failure };
            }
            co_return std::string{ self->m_read_buffer.data(), count };
        }

        /**
         * @brief Send with owned payload storage until all partial writes complete or fail.
         * @param self Owned connection for the complete coroutine lifetime.
         * @param bytes Owned send storage.
         * @param timeout Optional whole-transfer deadline.
         * @return Success only after every byte has been sent.
         */
        static auto writeAll(
            std::shared_ptr<Connection> self,
            std::string bytes,
            std::optional<Duration> timeout
        ) -> asio::awaitable<Result<void>> {
            co_await asio::this_coro::throw_if_cancelled(false);
            auto written = co_await writeWithProgress(std::move(self), std::move(bytes), timeout);
            if (written.error) { co_return std::unexpected{ *written.error }; }
            co_return Result<void>{};
        }

        /**
         * @brief Preserve the completed prefix through every deadline, close, and write failure.
         * @param self Owned connection for the complete coroutine lifetime.
         * @param bytes Owned send storage.
         * @param timeout Optional whole-transfer deadline.
         * @return The composed operation's byte count and classified failure, if any.
         */
        static auto writeWithProgress(
            std::shared_ptr<Connection> self,
            std::string bytes,
            std::optional<Duration> timeout
        ) -> asio::awaitable<WriteOutcome> {
            co_await asio::this_coro::throw_if_cancelled(false);
            if (self->m_closed || self->m_send_shutdown) { co_return WriteOutcome{ 0, Error::ConnectionClosed }; }
            if (self->m_writing) { co_return WriteOutcome{ 0, Error::Busy }; }
            if (bytes.size() > self->m_max_write_size) { co_return WriteOutcome{ 0, Error::BodyTooLarge }; }
            auto cancellation = co_await asio::this_coro::cancellation_state;
            if (cancellation.cancelled() != asio::cancellation_type::none) {
                self->close();
                co_return WriteOutcome{ 0, Error::Canceled };
            }
            if (bytes.empty()) { co_return WriteOutcome{}; }
            auto deadline = std::make_shared<Deadline>(self->m_executor);
            ActiveOperation active{ self->m_writing, deadline };
            self->armDeadline(deadline, timeout);
            if (deadline->expired) { co_return WriteOutcome{ 0, Error::WriteTimeout }; }
            auto const [error, count] = co_await asio::async_write(self->m_socket,
                asio::buffer(bytes), asio::as_tuple(asio::use_awaitable));
            if (deadline->expired) { co_return WriteOutcome{ count, Error::WriteTimeout }; }
            if (self->m_closed) { co_return WriteOutcome{ count, Error::Canceled }; }
            if (auto failure = map_io_error(error, IoOperation::Write)) {
                self->close();
                co_return WriteOutcome{ count, *failure };
            }
            if (count != bytes.size()) {
                self->close();
                co_return WriteOutcome{ count, Error::Write };
            }
            co_return WriteOutcome{ count };
        }

        /**
         * @brief Half-close only after the current send direction has drained.
         * @param self Owned connection for the complete coroutine lifetime.
         * @return Success, Busy while writing, or a contextual shutdown failure.
         */
        static auto shutdown(std::shared_ptr<Connection> self) -> asio::awaitable<Result<void>> {
            if (self->m_closed || self->m_send_shutdown) { co_return Result<void>{}; }
            if (self->m_writing) { co_return std::unexpected{ Error::Busy }; }
            asio::error_code error;
            self->m_socket.shutdown(asio::socket_base::shutdown_send, error);
            if (auto failure = map_io_error(error, IoOperation::Write)) { co_return std::unexpected{ *failure }; }
            self->m_send_shutdown = true;
            co_return Result<void>{};
        }
    };

} // namespace mhttp::impl
