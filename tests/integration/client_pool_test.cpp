/**
 * @file client_pool_test.cpp
 * @brief Exercise pooled persistent connections and loan lifetime through streaming and inherited cancellation.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"
#include "../support/thread_task.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Start a configured listener on a dynamic loopback port.
 * @param server Server retained through client completion.
 * @return Bound port.
 */
auto start_pool_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "pool fixture start failed" }; }
    return *port;
}

/**
 * @brief Observe a thread-safe test condition without unbounded blocking.
 * @param predicate Thread-safe observation.
 * @return Whether the condition became true within one second.
 */
auto pool_eventually(auto predicate) -> bool {
    auto const until = std::chrono::steady_clock::now() + 1s;
    while (!predicate() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(1ms); }
    return predicate();
}

/**
 * @brief Count the full lifetime of an awaited receiver frame, including cancellation cleanup.
 */
struct ReceiverFrame {
    std::atomic<unsigned>& active; ///< Counter outlives the transfer.
    /**
     * @brief Publish entry into a receiver.
     * @param value Counter borrowed through frame destruction.
     */
    explicit ReceiverFrame(std::atomic<unsigned>& value) : active{ value } { ++active; }
    /**
     * @brief Publish receiver cleanup before its loan may return.
     */
    ~ReceiverFrame() { --active; }
};

/**
 * @brief Reuse the same live socket and client configuration, reconnecting transparently after server idle closure.
 */
auto persistent_borrowing() -> void {
    for (bool validate : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start pooled persistence runtime");
        mhttp::server::HttpServer server{ runtime, { .keep_alive_timeout = 40ms } };
        Check(server.set_http_handler<mhttp::method::Get>("/", [](auto& request, auto& response) {
            Check(response.Headers().Set("X-Authorization", request.Header("Authorization")).has_value(), "echo pooled defaults");
            Check(response.SetContent("pooled", "text/plain").has_value(), "pooled response");
        }).has_value(), "register pooled endpoint");
        auto port = start_pool_server(server);
        mhttp::client::ClientPool pool{ runtime, { .max_per_origin = 1, .validate_on_borrow = validate } };
        auto first = mhttp::SyncWait(runtime, pool.Acquire(std::format("http://127.0.0.1:{}/ignored?q=1", port)));
        if (!Check(first.has_value(), "borrow real pooled client")) { continue; }
        Check((*first)->SetBearerToken("shared").has_value(), "configure persistent pooled defaults");
        auto initial = mhttp::SyncWait(runtime, (*first)->Get());
        auto* wrapper = first->Get();
        first->Release();
        auto next = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port));
        auto response = mhttp::SyncWait(runtime, (*next)->Get());
        Check(next->Get() == wrapper && initial && response && initial->LocalEndpoint() == response->LocalEndpoint(), "returned client reuses its persistent TCP socket");
        Check(response && response->Body() == "pooled" && response->Header("X-Authorization") == "Bearer shared", "borrowed URL target is root and defaults survive return");
        next->Release();
        std::this_thread::sleep_for(100ms);
        auto closed = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port));
        auto recovered = mhttp::SyncWait(runtime, (*closed)->Get());
        Check(recovered && initial && recovered->LocalEndpoint() != initial->LocalEndpoint(), "server-closed idle socket reconnects on the next request");
    }
}

/**
 * @brief Keep an exclusive loan until a receiver completes after its public handle has already been released.
 */
auto streaming_loan_lifetime() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start streaming pool runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Get>("/", [](auto&, auto& response) { static_cast<void>(response.SetContent("payload", "text/plain")); }).has_value(), "register pooled stream endpoint");
    auto port = start_pool_server(server);
    mhttp::client::ClientPool pool{ runtime, { .max_per_origin = 1 } };
    auto handle = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port));
    std::binary_semaphore entered{ 0 };
    std::atomic<bool> finish{};
    std::atomic<unsigned> active{};
    mhttp::client::TransferCallbacks callbacks;
    callbacks.on_body = [&](std::string chunk) -> mhttp::Task<mhttp::Result<void>> {
        ReceiverFrame frame{ active };
        CheckEq(chunk, std::string{ "payload" }, "receiver owns its body chunk");
        entered.release();
        while (!finish.load()) { co_await mhttp::Sleep(1ms); }
        co_return mhttp::Result<void>{};
    };
    auto issued = (*handle)->Send(mhttp::client::Request{}, std::move(callbacks));
    handle->Release();
    mhttp::test::ThreadTask transfer{ [task = std::move(issued), &runtime]() mutable {
        return mhttp::SyncWait(runtime, std::move(task));
    } };
    Check(entered.try_acquire_for(1s), "stream receiver awaits after handle release");
    auto denied = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port, 0ms));
    Check(!denied && denied.error() == mhttp::Error::AcquireTimeout && pool.Stats().active == 1 && active == 1, "receiver retains the exclusive slot until its frame drains");
    finish.store(true);
    auto complete = transfer.Get();
    Check(complete && complete->Body().empty() && active == 0 && pool.Stats().idle == 1, "completed receiver returns its loan after cleanup");
    auto reused = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port));
    auto next = mhttp::SyncWait(runtime, (*reused)->Get());
    Check(complete && next && complete->LocalEndpoint() == next->LocalEndpoint(), "stream-complete socket is reusable by the next borrower");
}

/**
 * @brief Cancel an awaited receiver via pool and runtime shutdown, releasing its final loan after frame cleanup.
 */
auto streaming_shutdown() -> void {
    for (bool stop_runtime : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start pooled receiver stop runtime");
        mhttp::server::HttpServer server{ runtime };
        Check(server.set_http_handler<mhttp::method::Get>("/", [](auto&, auto& response) { static_cast<void>(response.SetContent("body", "text/plain")); }).has_value(), "register pooled stop endpoint");
        auto port = start_pool_server(server);
        mhttp::client::ClientPool pool{ runtime, { .client_options = { .timeout_policy = mhttp::client::TimeoutPolicy::Never } } };
        auto handle = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port));
        std::binary_semaphore entered{ 0 };
        std::atomic<unsigned> active{};
        mhttp::client::TransferCallbacks callbacks;
        callbacks.on_body = [&](std::string) -> mhttp::Task<mhttp::Result<void>> {
            ReceiverFrame frame{ active };
            entered.release();
            co_await mhttp::Sleep(5s);
            co_return mhttp::Result<void>{};
        };
        auto issued = (*handle)->Send(mhttp::client::Request{}, std::move(callbacks));
        handle->Release();
        mhttp::test::ThreadTask transfer{ [task = std::move(issued), &runtime]() mutable {
            return mhttp::SyncWait(runtime, std::move(task));
        } };
        Check(entered.try_acquire_for(1s), "receiver entered before stop");
        if (stop_runtime) { runtime.Stop(); }
        else { pool.Stop(); }
        auto canceled = transfer.Get();
        Check(!canceled && canceled.error() == mhttp::Error::Canceled && active == 0, "stop drains the receiver before reporting cancellation");
        Check(pool.Stopped() && pool.Stats().Total() == 0, "final transfer owner retires stopped pool slot");
    }
}

/**
 * @brief Cancel a queued acquisition through its calling server session without stopping the pool or runtime.
 */
auto inherited_acquire_cancellation() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start inherited pool runtime");
    mhttp::client::ClientPool pool{ runtime, { .max_per_origin = 1 } };
    auto held = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80));
    std::atomic<bool> canceled{};
    mhttp::server::HttpServer proxy{ runtime };
    Check(proxy.set_http_handler<mhttp::method::Get>("/", [&](auto&, auto&) -> mhttp::Task<void> {
        auto result = co_await pool.Acquire("localhost", 80, 5s);
        canceled.store(!result && result.error() == mhttp::Error::Canceled);
    }).has_value(), "register queued proxy handler");
    mhttp::client::HttpClient caller{ runtime, "127.0.0.1", start_pool_server(proxy) };
    mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(runtime, caller.Get()); } };
    Check(pool_eventually([&] { return pool.Stats().waiting == 1; }), "calling session queues capacity demand");
    Check(proxy.Stop().has_value(), "stop calling session and drain its acquisition");
    auto failed = request.Get();
    Check(!failed && canceled && pool.Stats().waiting == 0 && !pool.Stopped(), "caller cancellation removes its waiter and preserves the pool");
    held->Release();
    auto recovered = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80, 0ms));
    Check(recovered && pool.Stats().active == 1, "canceled waiter cannot steal the next returned slot");
}

/**
 * @brief Return a timed-out connection's slot and reconnect through the next loan without losing capacity.
 */
auto timed_out_transfer_return() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start pooled timeout runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Get>("/slow", [](auto&, auto&) -> mhttp::Task<void> { co_await mhttp::Sleep(1s); }).has_value(), "register slow pooled response");
    Check(server.set_http_handler<mhttp::method::Get>("/ready", [](auto&, auto& response) { static_cast<void>(response.SetContent("ready", "text/plain")); }).has_value(), "register pool recovery route");
    auto port = start_pool_server(server);
    mhttp::client::ClientPool pool{ runtime, { .max_per_origin = 1, .client_options = { .read_timeout = 30ms } } };
    auto first = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port));
    auto failed = mhttp::SyncWait(runtime, (*first)->Get("/slow"));
    Check(!failed && failed.error() == mhttp::Error::ReadTimeout && !(*first)->IsOpen(), "response timeout closes borrowed socket");
    first->Release();
    auto next = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port, 0ms));
    if (!Check(next.has_value(), "failed transfer does not leak pool capacity")) { return; }
    auto recovered = mhttp::SyncWait(runtime, (*next)->Get("/ready"));
    Check(recovered && recovered->Body() == "ready", "returned wrapper reconnects after read timeout");
}

/**
 * @brief Run loopback pool integration cases.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "pooled persistent and closed connections", persistent_borrowing },
        { "streaming loan lifetime", streaming_loan_lifetime }, { "streaming pool shutdown", streaming_shutdown },
        { "inherited acquisition cancellation", inherited_acquire_cancellation },
        { "timed-out transfer return", timed_out_transfer_return },
    });
}
