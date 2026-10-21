/**
 * @file stream_writer.cppm
 * @brief Serialize bounded streamed writes, completion, and whole-response cancellation on a connection strand.
 */
module mhttp:server.impl.stream_writer;

import std;
import asio;
import :core.serializer;
import :core.impl.connection;

namespace mhttp::impl {

    /**
     * @brief Own one active response stream without borrowing its provider, request, or response.
     * @note Mutable state is confined to the connection strand. Retire before reusing the socket;
     *       abandoned lazy writes retain state but cannot write after retirement.
     */
    class StreamWriterImpl : public std::enable_shared_from_this<StreamWriterImpl> {
    private:
        using Clock = std::chrono::steady_clock; ///< Monotonic whole-response deadline clock.
        std::shared_ptr<Connection> m_connection; ///< Retain transport through every issued write.
        detail::ChunkEncoder m_encoder; ///< Own initial fields for terminal trailer validation.
        detail::BodyKind m_kind; ///< Fixed-length, chunked, or EOF framing selected before transmission.
        std::uint64_t m_remaining; ///< Unsent decoded bytes of a fixed-length representation.
        std::size_t m_chunk_bytes; ///< Maximum decoded size of one owned write.
        Clock::time_point m_deadline; ///< Shared deadline, including header and provider work.
        asio::steady_timer m_timer; ///< Cancel provider awaits even when no socket write is pending.
        std::stop_source m_stop; ///< Combine parent stop, expiry, and terminal stream failures.
        std::optional<Error> m_error; ///< First failure, preserved even if the provider ignores it.
        bool m_busy{}; ///< At most one write/finish may touch framing or the transport.
        bool m_finished{}; ///< Successful terminal operation, distinct from provider retirement.
        bool m_retired{}; ///< Provider frame drained; no issued operation may send more bytes.

        /**
         * @brief Own the validated session dependencies without starting callbacks from a constructor.
         * @param connection Owned connected transport, accessed on its strand.
         * @param kind Validated fixed-length, chunked, or EOF body framing.
         * @param headers Final initial fields, including defaults and Connection nominations.
         * @param chunk_bytes Positive decoded write bound.
         * @param deadline Absolute whole-response deadline already including header transmission.
         * @param content_length Exact decoded length when kind is ContentLength.
         */
        StreamWriterImpl(
            std::shared_ptr<Connection> connection,
            detail::BodyKind kind,
            Headers headers,
            std::size_t chunk_bytes,
            Clock::time_point deadline,
            std::uint64_t content_length
        ) : m_connection{ std::move(connection) }, m_encoder{ std::move(headers) }, m_kind{ kind },
            m_remaining{ content_length }, m_chunk_bytes{ chunk_bytes }, m_deadline{ deadline },
            m_timer{ m_connection->Executor() } {}

        /**
         * @brief Check stream state before modifying framing or sending bytes.
         * @return First failure, InvalidRequest after completion/retirement, Busy, timeout, or success.
         */
        auto ready() -> Result<void> {
            if (m_error) { return std::unexpected{ *m_error }; }
            if (m_retired || m_finished) { return std::unexpected{ Error::InvalidRequest }; }
            if (m_busy) { Fail(Error::Busy); return std::unexpected{ Error::Busy }; }
            if (Clock::now() >= m_deadline) {
                Fail(Error::WriteTimeout);
                return std::unexpected{ Error::WriteTimeout };
            }
            return {};
        }

        /**
         * @brief Hold the in-flight flag until the owned wire payload finishes on every exit path.
         * @param wire Owned encoded body or terminal bytes within the transport write bound.
         * @return Completion or the first terminal stream error.
         */
        auto send(std::string wire) -> asio::awaitable<Result<void>> {
            m_busy = true;
            /**
             * @brief Release the stream's in-flight flag on all coroutine completion paths.
             */
            struct BusyLease {
                bool& active; ///< Stream flag borrowed through the containing coroutine.
                /**
                 * @brief Clear the write flag even when the transport throws during cancellation.
                 */
                ~BusyLease() { active = false; }
            } lease{ m_busy };
            auto written = co_await m_connection->WriteAll(std::move(wire), m_deadline - Clock::now());
            if (!written) { Fail(written.error()); }
            if (m_error) { co_return std::unexpected{ *m_error }; }
            co_return Result<void>{};
        }

        /**
         * @brief Check one owned segment before allocating chunk framing and awaiting its whole write.
         * @param self Owned stream state, acquired before initial suspension.
         * @param bytes Owned binary content, never borrowed from application buffers.
         * @return Success for an empty open write or completed segment; failures prevent stream reuse.
         */
        static auto write(
            std::shared_ptr<StreamWriterImpl> self,
            std::string bytes
        ) -> asio::awaitable<Result<void>> {
            auto ready = self->ready();
            if (!ready) { co_return ready; }
            if (bytes.empty()) { co_return Result<void>{}; }
            if (self->m_kind == detail::BodyKind::ContentLength) {
                if (bytes.size() > self->m_remaining) {
                    self->Fail(Error::ProtocolError);
                    co_return std::unexpected{ Error::ProtocolError };
                }
                self->m_remaining -= bytes.size();
            }
            std::size_t overhead{};
            if (self->m_kind == detail::BodyKind::Chunked) {
                overhead = 4;
                for (auto value = bytes.size(); value != 0; value /= 16) { ++overhead; }
            }
            auto const bound = self->m_connection->MaxWriteSize();
            if (bytes.size() > self->m_chunk_bytes || overhead > bound || bytes.size() > bound - overhead) {
                self->Fail(Error::BodyTooLarge);
                co_return std::unexpected{ Error::BodyTooLarge };
            }
            if (self->m_kind == detail::BodyKind::Chunked) {
                auto encoded = self->m_encoder.Encode(bytes);
                co_return co_await self->send(std::move(*encoded));
            }
            co_return co_await self->send(std::move(bytes));
        }

        /**
         * @brief Write a bounded terminal chunk and optional safe trailers, or mark an EOF stream complete.
         * @param self Owned stream state, acquired before initial suspension.
         * @param trailers Owned validated fields; EOF streams cannot carry trailers.
         * @return Success only after terminal bytes are written; a failure is sticky and closes transport.
         */
        static auto finish(
            std::shared_ptr<StreamWriterImpl> self,
            Headers trailers
        ) -> asio::awaitable<Result<void>> {
            auto ready = self->ready();
            if (!ready) { co_return ready; }
            if (self->m_kind != detail::BodyKind::Chunked) {
                if (!trailers.Empty()) {
                    self->Fail(Error::InvalidHeaders);
                    co_return std::unexpected{ Error::InvalidHeaders };
                }
                if (self->m_kind == detail::BodyKind::ContentLength && self->m_remaining != 0) {
                    self->Fail(Error::ProtocolError);
                    co_return std::unexpected{ Error::ProtocolError };
                }
                self->m_finished = true;
                co_return Result<void>{};
            }
            auto remaining = self->m_connection->MaxWriteSize();
            if (remaining < 5) { self->Fail(Error::BodyTooLarge); co_return std::unexpected{ Error::BodyTooLarge }; }
            remaining -= 5;
            for (auto const& [name, value] : trailers) {
                for (auto size : { name.size(), value.size(), std::size_t{ 4 } }) {
                    if (size > remaining) {
                        self->Fail(Error::BodyTooLarge);
                        co_return std::unexpected{ Error::BodyTooLarge };
                    }
                    remaining -= size;
                }
            }
            auto terminal = self->m_encoder.Finish(trailers);
            if (!terminal) { self->Fail(terminal.error()); co_return std::unexpected{ terminal.error() }; }
            auto written = co_await self->send(std::move(*terminal));
            if (written) { self->m_finished = true; }
            co_return written;
        }

    public:
        /**
         * @brief Publish owned stream state and arm one deadline for the entire provider and output.
         * @param connection Owned connected transport on its strand.
         * @param kind Fixed-length, chunked, or EOF body framing.
         * @param headers Owned final fields used for trailer validation.
         * @param chunk_bytes Positive raw segment bound.
         * @param deadline Absolute phase deadline, including the already-written response head.
         * @param content_length Exact decoded length for fixed-length output, ignored otherwise.
         * @return Owned stream state whose timer cannot affect a later response after Retire.
         */
        static auto Create(
            std::shared_ptr<Connection> connection,
            detail::BodyKind kind,
            Headers headers,
            std::size_t chunk_bytes,
            Clock::time_point deadline,
            std::uint64_t content_length = 0
        ) -> std::shared_ptr<StreamWriterImpl> {
            auto state = std::shared_ptr<StreamWriterImpl>{ new StreamWriterImpl{
                std::move(connection), kind, std::move(headers), chunk_bytes, deadline, content_length } };
            state->m_timer.expires_at(deadline);
            state->m_timer.async_wait([weak = std::weak_ptr{ state }](asio::error_code error) {
                if (auto owner = weak.lock(); owner && !error && !owner->m_retired) { owner->Fail(Error::WriteTimeout); }
            });
            return state;
        }

        /**
         * @brief Borrow the stable executor used for public Task bridges and all state access.
         * @return Owned copy of the connection strand.
         */
        [[nodiscard]] auto Executor() const -> asio::any_io_executor { return m_connection->Executor(); }

        /**
         * @brief Observe combined provider cancellation for Sleep, Offload, and nested writes.
         * @return Stop token retained independently of the public writer wrapper.
         */
        [[nodiscard]] auto StopToken() const noexcept -> std::stop_token { return m_stop.get_token(); }

        /**
         * @brief Inspect the first failure on the connection strand after draining the provider.
         * @return Terminal failure if one occurred, including ignored write errors.
         */
        [[nodiscard]] auto ErrorValue() const noexcept -> std::optional<Error> { return m_error; }

        /**
         * @brief Inspect terminal completion on the connection strand.
         * @return Whether Finish has completed successfully.
         */
        [[nodiscard]] auto Finished() const noexcept -> bool { return m_finished; }

        /**
         * @brief Inspect a decoded segment size that fits both application and transport bounds.
         * @return Maximum safe raw Write size for the selected framing; zero if no segment can fit.
         */
        [[nodiscard]] auto MaxWriteSize() const noexcept -> std::size_t {
            auto size = std::min(m_chunk_bytes, m_connection->MaxWriteSize());
            if (m_kind != detail::BodyKind::Chunked) { return size; }
            for (;;) {
                std::size_t overhead{ 4 };
                for (auto value = size; value != 0; value /= 16) { ++overhead; }
                if (size == 0 || overhead <= m_connection->MaxWriteSize() - size) { return size; }
                --size;
            }
        }

        /**
         * @brief Record the first terminal failure and stop provider awaits before closing the socket.
         * @param error Owned backend-independent cause; call only on the connection strand.
         */
        auto Fail(Error error) -> void {
            if (m_retired || m_error) { return; }
            m_error = error;
            m_stop.request_stop();
            m_connection->Close();
        }

        /**
         * @brief Forward parent cancellation safely from any thread without retaining a public wrapper.
         */
        auto Cancel() -> void {
            asio::dispatch(Executor(), [self = shared_from_this()] { self->Fail(Error::Canceled); });
        }

        /**
         * @brief Retire the provider window and timer before permitting any later response on this socket.
         * @note Call on the connection strand after provider completion, including exceptional exits.
         */
        auto Retire() -> void {
            if (m_busy) { Fail(Error::Busy); }
            m_retired = true;
            static_cast<void>(m_timer.cancel());
        }

        /**
         * @brief Own a decoded segment and stream state before returning an unstarted write operation.
         * @param bytes Owned content, bounded and encoded only when awaited on the strand.
         * @return Operation that cannot send after provider retirement.
         */
        [[nodiscard]] auto Write(std::string bytes) -> asio::awaitable<Result<void>> {
            return asio::co_spawn(Executor(), write(shared_from_this(), std::move(bytes)), asio::use_awaitable);
        }

        /**
         * @brief Own terminal fields and stream state before returning an unstarted finish operation.
         * @param trailers Owned terminal fields; omitted by automatic successful-provider completion.
         * @return Completion operation serialized with all body writes.
         */
        [[nodiscard]] auto Finish(Headers trailers = {}) -> asio::awaitable<Result<void>> {
            return asio::co_spawn(Executor(), finish(shared_from_this(), std::move(trailers)), asio::use_awaitable);
        }
    };

    /**
     * @brief Retire stream state after its provider frame drains on every return or exception path.
     */
    struct StreamRetirement {
        std::shared_ptr<StreamWriterImpl> state; ///< Owned active state through coroutine cleanup.
        /**
         * @brief Neutralize the response deadline and reject escaped operations before socket reuse.
         */
        ~StreamRetirement() { state->Retire(); }
    };

} // namespace mhttp::impl
