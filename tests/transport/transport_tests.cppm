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
     * @brief Recover the known fixture io_context executor so new strands remain independent.
     * @param executor Executor created by TransportRuntime, possibly already wrapped in a strand.
     * @return The underlying io_context executor.
     */
    auto context_executor(asio::any_io_executor executor) -> asio::any_io_executor {
        return static_cast<asio::io_context&>(executor.context()).get_executor();
    }

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
        auto connection = impl::Connection::Create(context_executor(executor), limits);
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
     * @brief Cancel an idle monitor, reuse its socket, and distinguish data/EOF readiness without consumption.
     * @param executor Fixture strand.
     * @param weak Observe ownership after all wait callbacks drain.
     */
    auto idle_readiness_scenario(
        asio::any_io_executor executor,
        std::weak_ptr<impl::Connection>& weak
    ) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor);
        weak = pair->connection;
        asio::cancellation_signal cancel;
        auto waited = std::make_shared<TransportResult<Result<void>>>(executor);
        asio::co_spawn(executor, pair->connection->WaitForPeerData(), asio::bind_cancellation_slot(cancel.slot(),
            [waited](std::exception_ptr failure, Result<void> result) {
                waited->failure = failure;
                if (!failure) { waited->value.emplace(std::move(result)); }
                static_cast<void>(waited->done.try_send(asio::error_code{}));
            }));
        asio::steady_timer started{ executor };
        started.expires_after(10ms);
        co_await started.async_wait(asio::use_awaitable);
        auto overlap = co_await pair->connection->ReadSome(1s);
        Check(!overlap && overlap.error() == Error::Busy, "idle readiness and data reads share the receive guard");
        cancel.emit(asio::cancellation_type::all);
        auto canceled = co_await waited->Wait();
        Check(!canceled && canceled.error() == Error::Canceled, "idle wait cancellation drains without closing socket");
        co_await asio::async_write(pair->peer, asio::buffer("a", 1), asio::use_awaitable);
        auto readable = co_await pair->connection->WaitForPeerData();
        Check(readable.has_value(), "pending peer bytes produce readiness");
        auto data = co_await pair->connection->ReadSome(1s);
        Check(data && *data == "a", "readiness and prior cancellation leave every peer byte unconsumed");
        pair->peer.shutdown(asio::socket_base::shutdown_send);
        Check((co_await pair->connection->WaitForPeerData()).has_value(), "peer FIN produces readability");
        auto eof = co_await pair->connection->ReadSome(1s);
        Check(!eof && eof.error() == Error::ConnectionClosed, "normal read still observes the unconsumed FIN");
        pair->connection->Close();
    }

    /**
     * @brief Verify idle readiness ownership and cancellation on independent strands.
     */
    auto connection_idle_readiness() -> void {
        std::weak_ptr<impl::Connection> weak;
        {
            TransportRuntime runtime;
            runtime.Run(idle_readiness_scenario(runtime.Executor(), weak));
        }
        Check(weak.expired(), "idle wait callbacks and cancellation do not retain the connection after drain");
    }

    /**
     * @brief Reset a raw peer after observing enough bytes to ensure a composed write completed a prefix.
     * @param pair Owned pair retained until the peer has received and reset its socket.
     * @return Success after receiving the expected prefix.
     */
    auto reset_after_prefix(std::shared_ptr<LoopbackPair> pair) -> asio::awaitable<Result<void>> {
        std::string prefix(128 * 1024, '\0');
        co_await asio::async_read(pair->peer, asio::buffer(prefix), asio::use_awaitable);
        Check(std::ranges::all_of(prefix, [](char byte) { return byte == 'p'; }), "peer observes only the expected payload prefix");
        pair->peer.set_option(asio::socket_base::linger{ true, 0 });
        pair->peer.close();
        co_return Result<void>{};
    }

    /**
     * @brief Preserve complete, rejected, and failed-prefix counts without changing legacy write behavior.
     * @param executor Independent fixture strand.
     * @param weak Observe canceled write ownership after the runtime drains.
     */
    auto write_progress_scenario(
        asio::any_io_executor executor,
        std::weak_ptr<impl::Connection>& weak
    ) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor, { 16, 64 });
        auto reader = spawn_transport(executor, receive_peer(pair));
        auto empty = co_await pair->connection->WriteAllWithProgress("", 1s);
        Check(!empty.error && empty.bytes_transferred == 0, "empty successful write reports no transferred bytes");
        auto rejected = co_await pair->connection->WriteAllWithProgress(std::string(65, 'x'), 1s);
        Check(rejected.error == Error::BodyTooLarge && rejected.bytes_transferred == 0, "admission rejection reports no prefix and keeps socket usable");
        std::string payload{ "first\0second", 12 };
        auto complete = co_await pair->connection->WriteAllWithProgress(payload, 1s);
        Check(!complete.error && complete.bytes_transferred == payload.size(), "successful progress counts every binary payload byte");
        Check((co_await pair->connection->Shutdown()).has_value(), "shutdown after detailed write succeeds");
        CheckEq(co_await reader->Wait(), payload, "rejected writes contribute no wire bytes");
        auto closed = co_await pair->connection->WriteAllWithProgress("later", 1s);
        Check(closed.error == Error::ConnectionClosed && closed.bytes_transferred == 0, "closed direction reports no prefix");
        pair->connection->Close();

        auto reset_pair = co_await make_loopback(executor, { 16, 4 * 1024 * 1024 });
        reset_pair->connection->SocketForInitialization().set_option(asio::socket_base::send_buffer_size{ 4096 });
        auto resetting = spawn_transport(executor, reset_after_prefix(reset_pair));
        auto partial = co_await reset_pair->connection->WriteAllWithProgress(std::string(4 * 1024 * 1024, 'p'), 2s);
        Check(partial.error == Error::ConnectionClosed, "reset after payload prefix retains contextual write failure");
        Check(partial.bytes_transferred > 0 && partial.bytes_transferred < 4 * 1024 * 1024,
            "failed write reports its completed prefix instead of pretending nothing was sent");
        Check((co_await resetting->Wait()).has_value(), "join resetting peer before releasing sockets");
        auto after_failure = co_await reset_pair->connection->WriteAll("later", 1s);
        Check(!after_failure && after_failure.error() == Error::ConnectionClosed, "detailed write failure closes the shared legacy write path");

        auto cancel_pair = co_await make_loopback(executor, { 16, 4 * 1024 * 1024 });
        cancel_pair->connection->SocketForInitialization().set_option(asio::socket_base::send_buffer_size{ 1024 });
        cancel_pair->peer.set_option(asio::socket_base::receive_buffer_size{ 1024 });
        weak = cancel_pair->connection;
        asio::steady_timer closer{ executor };
        closer.expires_after(50ms);
        closer.async_wait([connection = cancel_pair->connection](asio::error_code error) {
            if (!error) { connection->Close(); }
        });
        auto canceled = co_await cancel_pair->connection->WriteAllWithProgress(std::string(4 * 1024 * 1024, 'x'), 2s);
        Check(canceled.error == Error::Canceled && canceled.bytes_transferred < 4 * 1024 * 1024,
            "closing a pending detailed write preserves cancellation and its incomplete count");
    }

    /**
     * @brief Verify detailed byte counts on a multithreaded loopback runtime.
     */
    auto connection_write_progress() -> void {
        std::weak_ptr<impl::Connection> weak;
        {
            TransportRuntime runtime;
            runtime.Run(write_progress_scenario(runtime.Executor(), weak));
        }
        Check(weak.expired(), "canceled detailed write releases connection and timer ownership after drain");
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
    ) -> asio::awaitable<impl::WriteOutcome> {
        std::string bytes(4 * 1024 * 1024, 'x');
        static_cast<void>(entered.try_send(asio::error_code{}));
        co_return co_await pair->connection->WriteAllWithProgress(std::move(bytes), 200ms);
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
        auto detailed_duplicate = co_await pair->connection->WriteAllWithProgress("duplicate", 1s);
        Check(detailed_duplicate.error == Error::Busy && detailed_duplicate.bytes_transferred == 0,
            "detailed and legacy writes share the in-flight guard without sending another prefix");
        auto premature_shutdown = co_await pair->connection->Shutdown();
        Check(!premature_shutdown && premature_shutdown.error() == Error::Busy, "shutdown cannot truncate a pending write");
        auto written = co_await writer->Wait();
        Check(written.error == Error::WriteTimeout, "nonreading peer hits whole-write deadline");
        Check(written.bytes_transferred < 4 * 1024 * 1024, "write timeout preserves the incomplete transfer count");
        Check(std::chrono::steady_clock::now() - started < 1s, "blocked write completes within timeout tolerance");
        auto read = co_await reader->Wait();
        Check(!read && read.error() == Error::Canceled, "write timeout cancels the other pending direction");
        auto expired = co_await make_loopback(executor);
        auto immediate = co_await expired->connection->WriteAllWithProgress("expired", -1ms);
        Check(immediate.error == Error::WriteTimeout && immediate.bytes_transferred == 0, "negative write deadline expires before sending");
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
     * @brief Test bridge storage without default construction, copying, or assignment.
     */
    struct BridgeValue {
        std::unique_ptr<int> value; ///< Owned transferred value.

        /**
         * @brief Construct one owned result.
         * @param input Scalar to own.
         */
        explicit BridgeValue(int input) : value{ std::make_unique<int>(input) } {}

        /**
         * @brief Move into the next bridge owner.
         * @param other Previous owner.
         */
        BridgeValue(BridgeValue&& other) noexcept = default;

        /**
         * @brief Require result construction instead of assignment.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(BridgeValue&& other) -> BridgeValue& = delete;
    };

    /**
     * @brief Produce a nondefault result after an asynchronous yield on a foreign strand.
     * @param on_foreign Predicate checking the actual target strand is executing.
     * @return Owned move-only result.
     */
    auto foreign_value(std::function<bool()> on_foreign) -> asio::awaitable<BridgeValue> {
        co_await asio::post(asio::use_awaitable);
        Check(on_foreign(), "Asio value operation executes on its target strand");
        co_return BridgeValue{ 42 };
    }

    /**
     * @brief Throw after a foreign asynchronous yield.
     * @return Never returns successfully.
     */
    auto foreign_failure() -> asio::awaitable<int> {
        co_await asio::post(asio::use_awaitable);
        throw std::range_error{ "foreign bridge failure" };
        co_return 0;
    }

    /**
     * @brief Cross into a foreign Asio operation and resume on the Task's original root strand.
     * @param foreign Executor independent of the Task root and its Asio caller.
     * @param on_root Predicate checking actual root execution.
     * @param on_foreign Predicate checking actual target execution.
     * @return Move-only result transferred in both bridge directions.
     */
    auto bridge_value_task(
        asio::any_io_executor foreign,
        std::function<bool()> on_root,
        std::function<bool()> on_foreign
    ) -> Task<BridgeValue> {
        Check(on_root(), "Task begins on its assigned strand");
        auto value = co_await impl::ToTask(foreign, foreign_value(std::move(on_foreign)));
        Check(on_root(), "foreign value resumes Task on the original strand");
        bool caught{};
        try { static_cast<void>(co_await impl::ToTask(foreign, foreign_failure())); }
        catch (std::range_error const& error) {
            caught = std::string_view{ error.what() } == "foreign bridge failure";
        }
        Check(caught && on_root(), "foreign exception retains its type and resumes the original strand");
        co_return value;
    }

    /**
     * @brief Preserve a foreign exception through the Task root back into an Asio caller.
     * @param foreign Foreign operation executor.
     * @return Never returns normally.
     */
    auto bridge_failure_task(asio::any_io_executor foreign) -> Task<int> {
        co_return co_await impl::ToTask(foreign, foreign_failure());
    }

    /**
     * @brief Repeatedly cross caller/root/operation strands and verify both result and exception routes.
     * @param caller Fixture executor.
     * @param on_caller Predicate checking the waiting Asio caller resumes on its own strand.
     */
    auto bridge_scenario(
        asio::any_io_executor caller,
        std::function<bool()> on_caller
    ) -> asio::awaitable<void> {
        auto root = asio::make_strand(context_executor(caller));
        auto foreign = asio::make_strand(context_executor(caller));
        for (unsigned i{}; i < 100; ++i) {
            auto value = co_await impl::AwaitTask(bridge_value_task(foreign,
                [root] { return root.running_in_this_thread(); },
                [foreign] { return foreign.running_in_this_thread(); }), root);
            CheckEq(*value.value, 42, "nondefault result survives both bridges");
            Check(on_caller(), "AwaitTask value returns to the original Asio caller");
            bool caught{};
            try { static_cast<void>(co_await impl::AwaitTask(bridge_failure_task(foreign), root)); }
            catch (std::range_error const& error) {
                caught = std::string_view{ error.what() } == "foreign bridge failure";
            }
            Check(caught && on_caller(), "root exception returns to the original Asio caller");
        }
    }

    /**
     * @brief Verify bridges when all three strands can run on different runtime threads.
     */
    auto bridge_executors() -> void {
        TransportRuntime runtime;
        auto caller = asio::make_strand(context_executor(runtime.Executor()));
        runtime.Run(asio::co_spawn(caller,
            bridge_scenario(caller, [caller] { return caller.running_in_this_thread(); }), asio::use_awaitable));
    }

    /**
     * @brief Suspend a Task in socket I/O after allowing a foreign thread to request root cancellation.
     * @param connection Owned connection retained across bridge suspension.
     * @param entered Readiness notification borrowed until the root finishes.
     * @return Socket result; pre-initiation cancellation may propagate as a standard exception.
     */
    auto canceled_connection_task(
        std::shared_ptr<impl::Connection> connection,
        std::latch& entered
    ) -> Task<Result<std::string>> {
        entered.count_down();
        co_return co_await impl::ToTask(connection->Executor(), connection->ReadSome(5s));
    }

    /**
     * @brief Cancel socket I/O through a Task root token, including operation initiation races.
     * @param executor Fixture executor.
     * @param stop Token whose source is requested from a foreign thread.
     * @param entered Root entry notification for that thread.
     * @param weak Connection lifetime observation after completion drain.
     */
    auto cancellation_scenario(
        asio::any_io_executor executor,
        std::stop_token stop,
        std::latch& entered,
        std::weak_ptr<impl::Connection>& weak
    ) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor);
        weak = pair->connection;
        auto root = asio::make_strand(context_executor(executor));
        Error observed{ Error::Unknown };
        auto const started = std::chrono::steady_clock::now();
        try {
            auto read = co_await impl::AwaitTask(canceled_connection_task(pair->connection, entered), root, stop);
            Check(!read, "cancellation cannot report successful quiet socket I/O");
            if (!read) { observed = read.error(); }
        } catch (std::system_error const& error) {
            CheckEq(error.code(), make_error_code(Error::Canceled), "pre-initiation cancellation keeps mhttp category");
            if (error.code() == Error::Canceled) { observed = Error::Canceled; }
        }
        CheckEq(observed, Error::Canceled, "root cancellation reaches the nested connection operation");
        Check(std::chrono::steady_clock::now() - started < 1s, "root cancellation does not wait for the read deadline");
        pair->connection->Close();
    }

    /**
     * @brief Repeatedly cancel roots from outside I/O execution and check socket/timer owners drain.
     */
    auto connection_cancellation() -> void {
        std::array<std::weak_ptr<impl::Connection>, 20> observations;
        {
            TransportRuntime runtime;
            for (auto& weak : observations) {
                std::stop_source stop;
                std::latch entered{ 1 };
                std::jthread stopper{ [&stop, &entered] {
                    entered.wait();
                    stop.request_stop();
                } };
                runtime.Run(cancellation_scenario(runtime.Executor(), stop.get_token(), entered, weak));
                stopper.join();
            }
        }
        for (auto const& weak : observations) { Check(weak.expired(), "canceled root releases connection state"); }
    }

    /**
     * @brief Verify returned lazy Asio operations retain the connection before initial suspension.
     * @param executor Fixture executor.
     * @param weak Lifetime observation surviving fixture drain.
     */
    auto ownership_scenario(
        asio::any_io_executor executor,
        std::weak_ptr<impl::Connection>& weak
    ) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor);
        weak = pair->connection;
        auto pending = pair->connection->ReadSome(1s);
        pair->connection.reset();
        Check(!weak.expired(), "unstarted read operation retains connection ownership");
        co_await asio::async_write(pair->peer, asio::buffer("a", 1), asio::use_awaitable);
        auto read = co_await std::move(pending);
        Check(read && *read == "a", "read completes after every external connection owner was dropped");
        CheckEq(co_await receive_peer(pair), std::string{}, "last operation owner closes its socket on destruction");
        auto reset = co_await make_loopback(executor);
        reset->peer.set_option(asio::socket_base::linger{ true, 0 });
        reset->peer.close();
        auto disconnected = co_await reset->connection->ReadSome(1s);
        Check(!disconnected && disconnected.error() == Error::ConnectionClosed, "peer reset is a stable closure error");
    }

    /**
     * @brief Check ownership through lazy start, completion, and peer disconnect.
     */
    auto connection_ownership() -> void {
        std::weak_ptr<impl::Connection> weak;
        {
            TransportRuntime runtime;
            runtime.Run(ownership_scenario(runtime.Executor(), weak));
        }
        Check(weak.expired(), "last operation releases socket and retired timer owners");
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
            { "connection idle readiness", connection_idle_readiness },
            { "connection write progress", connection_write_progress },
            { "connection read deadlines", connection_read_deadlines },
            { "connection write deadlines", connection_write_deadlines },
            { "connection close", connection_close },
            { "bridge independent executors", bridge_executors },
            { "connection root cancellation", connection_cancellation },
            { "connection operation ownership", connection_ownership },
        });
    }

} // namespace mhttp::test
