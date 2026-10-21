/**
 * @file transport_tests.cppm
 * @brief Exercise private transport behavior through a fixture exported only in a temporary project.
 */
export module mhttp:test.transport;

import std;
import asio;
import :core.error;
import :core.impl.io_error;
import :core.impl.runtime;
import :core.impl.bridge;
import :core.impl.connection;

#include "test_support.hpp"

namespace mhttp::test {

    using namespace std::chrono_literals;

    /**
     * @brief Own a multithreaded runtime for one bounded transport scenario.
     */
    class TransportRuntime {
    private:
        std::shared_ptr<impl::RuntimeImpl> m_runtime{ std::make_shared<impl::RuntimeImpl>() }; ///< Runtime owner.
        asio::any_io_executor m_executor{ asio::make_strand(m_runtime->Context()) }; ///< Fixture strand.

    public:
        /**
         * @brief Start enough I/O threads to expose accidental cross-strand socket access.
         */
        TransportRuntime() { Check(m_runtime->Start(4).has_value(), "start transport runtime"); }

        /**
         * @brief Drain timer and socket completions before destroying the runtime.
         */
        ~TransportRuntime() {
            m_runtime->Stop();
            Check(m_runtime->Join().has_value(), "drain transport runtime");
        }

        /**
         * @brief Borrow the fixture executor independently of connection executors.
         * @return Fixture strand.
         */
        auto Executor() const -> asio::any_io_executor { return m_executor; }

        /**
         * @brief Wait outside I/O execution, preserving the scenario's exception.
         * @param operation Owned scenario; its arguments must outlive completion.
         */
        auto Run(asio::awaitable<void> operation) -> void {
            std::latch done{ 1 };
            std::exception_ptr failure;
            asio::co_spawn(m_executor, std::move(operation), [&done, &failure](std::exception_ptr error) {
                failure = error;
                done.count_down();
            });
            done.wait();
            if (failure) { std::rethrow_exception(failure); }
        }
    };

    /**
     * @brief Retain a spawned fixture operation's result until its parent explicitly joins it.
     * @tparam T Owned, default-constructible fixture result type.
     */
    template <typename T>
    struct TransportResult {
        asio::experimental::channel<void(asio::error_code)> done; ///< One buffered completion.
        std::exception_ptr failure; ///< Original spawned operation exception.
        std::optional<T> value; ///< Completed result.

        /**
         * @brief Bind result publication and joining to the fixture strand.
         * @param executor Fixture executor.
         */
        explicit TransportResult(asio::any_io_executor executor) : done{ executor, 1 } {}

        /**
         * @brief Join a child before the fixture drops its socket or buffer owners.
         * @return Completed result, rethrowing the child's exception if it failed.
         */
        auto Wait() -> asio::awaitable<T> {
            co_await done.async_receive(asio::use_awaitable);
            if (failure) { std::rethrow_exception(failure); }
            co_return std::move(*value);
        }
    };

    /**
     * @brief Start a fixture operation with an exception-aware completion and explicit join state.
     * @tparam T Owned fixture result type.
     * @param executor Fixture strand, also used for completion publication.
     * @param operation Owned child coroutine.
     * @return Join state retained by both parent and completion callback.
     */
    template <typename T>
    auto spawn_transport(
        asio::any_io_executor executor,
        asio::awaitable<T> operation
    ) -> std::shared_ptr<TransportResult<T>> {
        auto result = std::make_shared<TransportResult<T>>(executor);
        asio::co_spawn(executor, std::move(operation), [result](std::exception_ptr failure, T value) {
            result->failure = failure;
            if (!failure) { result->value.emplace(std::move(value)); }
            static_cast<void>(result->done.try_send(asio::error_code{}));
        });
        return result;
    }

    /**
     * @brief Own both ends of a dynamically bound loopback connection.
     */
    struct LoopbackPair {
        std::shared_ptr<impl::Connection> connection; ///< Transport under test.
        asio::ip::tcp::socket peer; ///< Raw peer owned through all fixture operations.

        /**
         * @brief Create the peer on the independent fixture strand.
         * @param executor Fixture executor.
         * @param transport Connection under test.
         */
        LoopbackPair(asio::any_io_executor executor, std::shared_ptr<impl::Connection> transport) :
            connection{ std::move(transport) }, peer{ executor } {}
    };

    /**
     * @brief Connect the raw peer asynchronously while the parent accepts it.
     * @param pair Owned peer and transport lifetimes.
     * @param endpoint Dynamic loopback endpoint.
     * @return Successful connection establishment.
     */
    auto connect_peer(
        std::shared_ptr<LoopbackPair> pair,
        asio::ip::tcp::endpoint endpoint
    ) -> asio::awaitable<Result<void>> {
        co_await pair->peer.async_connect(endpoint, asio::use_awaitable);
        co_return Result<void>{};
    }

    /**
     * @brief Establish a private connection and raw peer without using any public network service.
     * @param executor Fixture executor.
     * @param limits Transport limits to exercise.
     * @return Both connected socket owners.
     */
    auto make_loopback(
        asio::any_io_executor executor,
        impl::ConnectionLimits limits = {}
    ) -> asio::awaitable<std::shared_ptr<LoopbackPair>> {
        asio::ip::tcp::acceptor acceptor{ executor, { asio::ip::address_v4::loopback(), 0 } };
        auto connection = impl::Connection::Create(executor, limits);
        if (!connection) { throw std::invalid_argument{ "invalid fixture connection limits" }; }
        auto pair = std::make_shared<LoopbackPair>(executor, std::move(*connection));
        auto connected = spawn_transport(executor, connect_peer(pair, acceptor.local_endpoint()));
        co_await acceptor.async_accept(pair->connection->SocketForInitialization(), asio::use_awaitable);
        Check((co_await connected->Wait()).has_value(), "loopback peer connects");
        co_return pair;
    }

    /**
     * @brief Send binary data in deliberately small independently awaited writes, then half-close.
     * @param pair Connected peer, owned until all writes complete.
     * @param bytes Owned test payload.
     * @return Successful complete transmission.
     */
    auto send_parts(
        std::shared_ptr<LoopbackPair> pair,
        std::string bytes
    ) -> asio::awaitable<Result<void>> {
        for (std::size_t position{}; position < bytes.size();) {
            auto const count = std::min<std::size_t>(5, bytes.size() - position);
            co_await asio::async_write(pair->peer, asio::buffer(bytes.data() + position, count), asio::use_awaitable);
            position += count;
        }
        pair->peer.shutdown(asio::socket_base::shutdown_send);
        co_return Result<void>{};
    }

    /**
     * @brief Collect peer bytes through many bounded reads until the transport sends FIN.
     * @param pair Connected peer retained until EOF.
     * @return Owned wire bytes.
     */
    auto receive_peer(std::shared_ptr<LoopbackPair> pair) -> asio::awaitable<std::string> {
        std::string bytes;
        std::array<char, 257> buffer{};
        for (;;) {
            auto const [error, count] = co_await pair->peer.async_read_some(
                asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
            bytes.append(buffer.data(), count);
            if (error == asio::error::eof) { break; }
            if (error) { throw std::system_error{ error }; }
        }
        co_return bytes;
    }

    /**
     * @brief Check invalid/maximum limits before any network transfer starts.
     */
    auto connection_limits() -> void {
        asio::io_context context;
        Check(!impl::Connection::Create(context.get_executor(), { 0, 1 }), "zero read buffer is rejected");
        Check(!impl::Connection::Create(context.get_executor(),
            { impl::ConnectionLimits::MAX_READ_BUFFER_SIZE + 1, 1 }), "over-limit read buffer is rejected");
        Check(!impl::Connection::Create(context.get_executor(), { 1, 0 }), "zero write limit is rejected");
        Check(impl::Connection::Create(context.get_executor(),
            { impl::ConnectionLimits::MAX_READ_BUFFER_SIZE, 1 }).has_value(), "maximum read buffer is accepted");
    }

    /**
     * @brief Check owned read chunks, wire fidelity, half-close, write limits, and send shutdown.
     * @param executor Fixture executor independent of the connection strand.
     */
    auto transfer_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor, { 7, 1024 * 1024 });
        pair->connection->SocketForInitialization().set_option(asio::socket_base::send_buffer_size{ 4096 });
        std::string const input{ "first\0second\r\nthird", 19 };
        auto sender = spawn_transport(executor, send_parts(pair, input));
        std::string received;
        std::optional<std::string> first;
        for (;;) {
            auto chunk = co_await pair->connection->ReadSome(1s);
            if (!chunk) {
                CheckEq(chunk.error(), Error::ConnectionClosed, "peer send half-close reports EOF");
                break;
            }
            Check(!chunk->empty() && chunk->size() <= 7, "each receive stays within configured scratch space");
            if (!first) { first = *chunk; }
            received += *chunk;
        }
        CheckEq(received, input, "binary fragmented input retains every byte");
        Check(first && received.starts_with(*first), "first owned result survives later reads");
        Check((co_await sender->Wait()).has_value(), "join peer writer");
        auto reader = spawn_transport(executor, receive_peer(pair));
        Check((co_await pair->connection->WriteAll("", 1s)).has_value(), "empty write succeeds without wire bytes");
        auto oversized = co_await pair->connection->WriteAll(std::string(1024 * 1024 + 1, 'x'), 1s);
        Check(!oversized && oversized.error() == Error::BodyTooLarge, "over-limit write sends no prefix");
        std::string output(1024 * 1024, 'z');
        for (std::size_t i{}; i < output.size(); i += 251) { output[i] = '\0'; }
        auto written = co_await pair->connection->WriteAll(output, 2s);
        Check(written.has_value(), "maximum-size owned write completes after peer half-close");
        Check((co_await pair->connection->Shutdown()).has_value(), "shutdown sends FIN after all bytes");
        Check((co_await pair->connection->Shutdown()).has_value(), "repeated shutdown is idempotent");
        auto closed_write = co_await pair->connection->WriteAll("later", 1s);
        Check(!closed_write && closed_write.error() == Error::ConnectionClosed, "writes after send shutdown fail");
        CheckEq(co_await reader->Wait(), output, "partial writes reproduce only the allowed payload");
        pair->connection->Close();
    }

    /**
     * @brief Run a fragmented bidirectional transfer on a multithreaded runtime.
     */
    auto connection_transfers() -> void {
        TransportRuntime runtime;
        runtime.Run(transfer_scenario(runtime.Executor()));
    }

    /**
     * @brief Delay a peer write beyond an earlier read's retired deadline.
     * @param pair Owned connected pair.
     * @param delay Relative send delay.
     * @return Successful single-byte transmission.
     */
    auto delayed_send(
        std::shared_ptr<LoopbackPair> pair,
        std::chrono::steady_clock::duration delay
    ) -> asio::awaitable<Result<void>> {
        asio::steady_timer timer{ co_await asio::this_coro::executor };
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);
        co_await asio::async_write(pair->peer, asio::buffer("b", 1), asio::use_awaitable);
        co_return Result<void>{};
    }

    /**
     * @brief Check real/disabled/immediate deadlines and repeated reuse after timer cancellation.
     * @param executor Fixture executor.
     * @param weak Lifetime observation that survives runtime drain.
     */
    auto read_deadline_scenario(
        asio::any_io_executor executor,
        std::weak_ptr<impl::Connection>& weak
    ) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor);
        for (unsigned i{}; i < 12; ++i) {
            co_await asio::async_write(pair->peer, asio::buffer("a", 1), asio::use_awaitable);
            auto first = co_await pair->connection->ReadSome(25ms);
            Check(first && *first == "a", "already queued bytes beat the first deadline");
            auto sender = spawn_transport(executor, delayed_send(pair, 35ms));
            auto second = co_await pair->connection->ReadSome(std::nullopt);
            Check(second && *second == "b", "retired deadline cannot close a later pending read");
            Check((co_await sender->Wait()).has_value(), "join delayed peer write");
        }
        weak = pair->connection;
        auto const started = std::chrono::steady_clock::now();
        auto timed_out = co_await pair->connection->ReadSome(25ms);
        auto const elapsed = std::chrono::steady_clock::now() - started;
        Check(!timed_out && timed_out.error() == Error::ReadTimeout, "quiet peer hits read deadline");
        Check(elapsed >= 15ms && elapsed < 1s, "read timeout has bounded timing tolerance");
        auto after = co_await pair->connection->ReadSome(1s);
        Check(!after && after.error() == Error::ConnectionClosed, "timed-out socket cannot be reused");
        auto expired = co_await make_loopback(executor);
        auto immediate = co_await expired->connection->ReadSome(0ms);
        Check(!immediate && immediate.error() == Error::ReadTimeout, "nonpositive read deadline expires immediately");
    }

    /**
     * @brief Verify read timers retire without leaking owners or affecting later operations.
     */
    auto connection_read_deadlines() -> void {
        std::weak_ptr<impl::Connection> weak;
        {
            TransportRuntime runtime;
            runtime.Run(read_deadline_scenario(runtime.Executor(), weak));
        }
        Check(weak.expired(), "read timeout releases connection after completion drain");
    }

    /**
     * @brief Announce a read on the fixture strand before its nested connection coroutine suspends.
     * @param pair Owned connected pair.
     * @param entered Readiness channel on the fixture strand.
     * @return Result of a read long enough to be canceled externally.
     */
    auto announced_read(
        std::shared_ptr<LoopbackPair> pair,
        asio::experimental::channel<void(asio::error_code)>& entered
    ) -> asio::awaitable<Result<std::string>> {
        static_cast<void>(entered.try_send(asio::error_code{}));
        co_return co_await pair->connection->ReadSome(5s);
    }

    /**
     * @brief Announce a large write before it blocks against the deliberately nonreading peer.
     * @param pair Owned connected pair.
     * @param entered Readiness channel on the fixture strand.
     * @return Whole-transfer result after completion or deadline.
     */
    auto announced_write(
        std::shared_ptr<LoopbackPair> pair,
        asio::experimental::channel<void(asio::error_code)>& entered
    ) -> asio::awaitable<Result<void>> {
        std::string bytes(4 * 1024 * 1024, 'x');
        static_cast<void>(entered.try_send(asio::error_code{}));
        co_return co_await pair->connection->WriteAll(std::move(bytes), 200ms);
    }

    /**
     * @brief Hold both directions active until the blocked write's deadline closes the connection.
     * @param executor Fixture executor.
     */
    auto write_deadline_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor, { 16, 4 * 1024 * 1024 });
        pair->connection->SocketForInitialization().set_option(asio::socket_base::send_buffer_size{ 1024 });
        pair->peer.set_option(asio::socket_base::receive_buffer_size{ 1024 });
        asio::experimental::channel<void(asio::error_code)> entered{ executor, 1 };
        auto reader = spawn_transport(executor, announced_read(pair, entered));
        co_await entered.async_receive(asio::use_awaitable);
        asio::experimental::channel<void(asio::error_code)> write_entered{ executor, 1 };
        auto const started = std::chrono::steady_clock::now();
        auto writer = spawn_transport(executor, announced_write(pair, write_entered));
        co_await write_entered.async_receive(asio::use_awaitable);
        auto duplicate = co_await pair->connection->WriteAll("duplicate", 1s);
        Check(!duplicate && duplicate.error() == Error::Busy, "overlapping writes are rejected before sending");
        auto premature_shutdown = co_await pair->connection->Shutdown();
        Check(!premature_shutdown && premature_shutdown.error() == Error::Busy, "shutdown cannot truncate a pending write");
        auto written = co_await writer->Wait();
        Check(!written && written.error() == Error::WriteTimeout, "nonreading peer hits whole-write deadline");
        Check(std::chrono::steady_clock::now() - started < 1s, "blocked write completes within timeout tolerance");
        auto read = co_await reader->Wait();
        Check(!read && read.error() == Error::Canceled, "write timeout cancels the other pending direction");
        auto expired = co_await make_loopback(executor);
        auto immediate = co_await expired->connection->WriteAll("expired", -1ms);
        Check(!immediate && immediate.error() == Error::WriteTimeout, "negative write deadline expires before sending");
        std::array<char, 32> unexpected{};
        auto const [error, count] = co_await expired->peer.async_read_some(
            asio::buffer(unexpected), asio::as_tuple(asio::use_awaitable));
        Check(count == 0 && error == asio::error::eof, "expired write sends no payload prefix");
    }

    /**
     * @brief Verify a blocked send cannot retain the connection past its deadline.
     */
    auto connection_write_deadlines() -> void {
        TransportRuntime runtime;
        runtime.Run(write_deadline_scenario(runtime.Executor()));
    }

    /**
     * @brief Check overlapping reads and cancellation by a caller outside the I/O runtime.
     * @param executor Fixture executor.
     * @param external Connection published before ready is signaled.
     * @param ready Synchronizes publication with the foreign closer.
     */
    auto close_scenario(
        asio::any_io_executor executor,
        std::shared_ptr<impl::Connection>& external,
        std::latch& ready
    ) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor);
        asio::experimental::channel<void(asio::error_code)> entered{ executor, 1 };
        auto reader = spawn_transport(executor, announced_read(pair, entered));
        co_await entered.async_receive(asio::use_awaitable);
        auto duplicate = co_await pair->connection->ReadSome(1s);
        Check(!duplicate && duplicate.error() == Error::Busy, "overlapping same-direction reads are rejected");
        external = pair->connection;
        ready.count_down();
        auto canceled = co_await reader->Wait();
        Check(!canceled && canceled.error() == Error::Canceled, "foreign close cancels an active read");
        auto after = co_await pair->connection->ReadSome(1s);
        Check(!after && after.error() == Error::ConnectionClosed, "future reads see closed state");
        Check((co_await pair->connection->Shutdown()).has_value(), "shutdown after close is safe");
        pair->connection->Close();
        pair->connection->Close();
        CheckEq(co_await receive_peer(pair), std::string{}, "close delivers clean peer EOF without bytes");
    }

    /**
     * @brief Verify foreign-thread close is posted safely and releases all owned operation state.
     */
    auto connection_close() -> void {
        std::shared_ptr<impl::Connection> external;
        std::weak_ptr<impl::Connection> weak;
        std::latch ready{ 1 };
        {
            TransportRuntime runtime;
            std::jthread closer{ [&external, &ready] {
                ready.wait();
                external->Close();
            } };
            runtime.Run(close_scenario(runtime.Executor(), external, ready));
            closer.join();
            weak = external;
            external.reset();
        }
        Check(weak.expired(), "foreign close releases connection after timer and socket drain");
    }

    /**
     * @brief Verify success, cancellation, deadline precedence, peer closure, and operation fallbacks.
     */
    auto io_error_mapping() -> void {
        using enum impl::IoOperation;
        for (auto operation : { Connect, Resolve, Bind, Read, Write }) {
            Check(!impl::map_io_error({}, operation), "successful completion has no error");
            CheckEq(impl::map_io_error(asio::error::operation_aborted, operation),
                std::optional{ Error::Canceled }, "external cancellation stays distinct from timeouts");
        }
        struct Case {
            asio::error_code error; ///< Backend outcome.
            impl::IoOperation operation; ///< Operation being performed.
            bool expired{}; ///< A winning local deadline.
            Error expected; ///< Stable public error.
        };
        Case const cases[]{
            { asio::error::timed_out, Connect, false, Error::ConnectionTimeout },
            { asio::error::timed_out, Read, false, Error::ReadTimeout },
            { asio::error::timed_out, Write, false, Error::WriteTimeout },
            { asio::error::operation_aborted, Read, true, Error::ReadTimeout },
            { asio::error::operation_aborted, Write, true, Error::WriteTimeout },
            { {}, Connect, true, Error::ConnectionTimeout },
            { asio::error::eof, Read, false, Error::ConnectionClosed },
            { asio::error::connection_reset, Read, false, Error::ConnectionClosed },
            { asio::error::connection_aborted, Read, false, Error::ConnectionClosed },
            { asio::error::broken_pipe, Write, false, Error::ConnectionClosed },
            { asio::error::not_connected, Write, false, Error::ConnectionClosed },
            { asio::error::shut_down, Write, false, Error::ConnectionClosed },
            { asio::error::connection_refused, Connect, false, Error::Connection },
            { asio::error::connection_reset, Connect, false, Error::Connection },
            { asio::error::host_not_found, Resolve, false, Error::ResolveFailed },
            { asio::error::timed_out, Resolve, false, Error::ResolveFailed },
            { asio::error::address_in_use, Bind, false, Error::BindIpAddress },
            { asio::error::timed_out, Bind, false, Error::BindIpAddress },
            { asio::error::fault, Read, false, Error::Read },
            { asio::error::fault, Write, false, Error::Write },
            { asio::error::fault, static_cast<impl::IoOperation>(999), false, Error::Unknown },
        };
        for (auto const& test : cases) {
            CheckEq(impl::map_io_error(test.error, test.operation, test.expired),
                std::optional{ test.expected }, "contextual transport error mapping");
        }
    }

} // namespace mhttp::test

export namespace mhttp::test {

    /**
     * @brief Run the private transport fixture without exporting it from the production library.
     * @param argc Argument count.
     * @param argv Case name filters.
     * @return Test runner exit status.
     */
    auto RunTransportTests(int argc, char* argv[]) -> int {
        return RunTests(argc, argv, {
            { "I/O error mapping", io_error_mapping },
            { "connection limits", connection_limits },
            { "connection transfers", connection_transfers },
            { "connection read deadlines", connection_read_deadlines },
            { "connection write deadlines", connection_write_deadlines },
            { "connection close", connection_close },
        });
    }

} // namespace mhttp::test
