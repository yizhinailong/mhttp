/**
 * @file graceful_stop_test.cpp
 * @brief Exercise graceful request completion, deadline escalation, concurrent callers, and owned cleanup.
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
 * @param server Retained server.
 * @return Bound port.
 */
auto start_grace_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "grace fixture failed" }; }
    return *port;
}

/**
 * @brief Observe a thread-safe lifecycle transition with a one-second bound.
 * @param predicate Thread-safe observation.
 * @return Whether the transition occurred.
 */
auto grace_eventually(auto predicate) -> bool {
    auto const until = std::chrono::steady_clock::now() + 1s;
    while (!predicate() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(1ms); }
    return predicate();
}

/**
 * @brief Track handler-frame lifetime through awaited cancellation and worker cleanup.
 */
struct GraceFrame {
    std::atomic<unsigned>& active; ///< Counter outlives every handler frame.
    /**
     * @brief Publish frame construction.
     * @param counter Borrowed longer-lived counter.
     */
    explicit GraceFrame(std::atomic<unsigned>& counter) : active{ counter } { ++active; }
    /**
     * @brief Publish final cleanup.
     */
    ~GraceFrame() { --active; }
};

/**
 * @brief Verify blocking grace overloads reject I/O callers without changing lifecycle state.
 * @param server Borrowed server, retained through this control Task.
 */
auto reject_blocking_grace(mhttp::server::HttpServer& server) -> mhttp::Task<void> {
    CheckEq(server.Stop(1s).error(), mhttp::Error::Busy, "blocking graceful Stop rejects I/O threads");
    co_return;
}

/**
 * @brief Reject invalid budgets and stop bound/idle targets without posting to an unstarted Runtime.
 */
auto grace_configuration() -> void {
    mhttp::Runtime caller_runtime;
    mhttp::Runtime target_runtime;
    Check(caller_runtime.Start(2).has_value(), "start independent grace caller");
    mhttp::server::HttpServer server{ target_runtime };
    Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind idle grace target");
    auto const bound = server.LocalEndpoint();
    CheckEq(server.Stop(-1ms).error(), mhttp::Error::InvalidRequest, "negative grace rejected");
    auto invalid = mhttp::SyncWait(caller_runtime, server.AsyncStop(std::chrono::steady_clock::duration::max()));
    Check(!invalid && invalid.error() == mhttp::Error::InvalidRequest && server.LocalEndpoint() == bound, "overflow rejection preserves bound socket");
    mhttp::SyncWait(caller_runtime, reject_blocking_grace(server));
    Check(mhttp::SyncWait(caller_runtime, server.AsyncStop(1s)).has_value(), "async graceful Stop handles bound-only idle target");
    Check(!server.LocalEndpoint(), "bound-only graceful Stop releases listener");
    Check(server.Stop(0ms).has_value(), "idle zero-grace Stop succeeds");
    Check(server.BindToAnyPort("127.0.0.1").has_value(), "rebind idle target for immediate async Stop");
    mhttp::SyncWait(caller_runtime, server.AsyncStop());
    Check(!server.LocalEndpoint(), "immediate async Stop also handles an unstarted target Runtime");
    mhttp::Task<mhttp::Result<void>> issued;
    {
        auto transient = std::make_unique<mhttp::server::HttpServer>(target_runtime);
        issued = transient->AsyncStop(1s);
    }
    Check(mhttp::SyncWait(caller_runtime, std::move(issued)).has_value(), "issued grace Task retains state after wrapper destruction");
}

/**
 * @brief Finish a begun request while rejecting new routing and preserve restart after deadline cleanup.
 */
auto graceful_completion() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start natural grace runtime");
    std::binary_semaphore entered{ 0 };
    std::atomic<bool> finish{};
    mhttp::server::HttpServer server{ runtime };
    Check(server.Get("/", [&](auto&, auto& response) -> mhttp::Task<void> {
        entered.release();
        while (!finish.load()) { co_await mhttp::Sleep(1ms); }
        static_cast<void>(response.SetContent("completed", "text/plain"));
    }).has_value(), "register graceful response");
    auto port = start_grace_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(runtime, client.Get()); } };
    Check(entered.try_acquire_for(1s), "request begins before grace");
    mhttp::test::ThreadTask stop{ [&] { return server.Stop(2s); } };
    Check(grace_eventually([&] { return !server.IsRunning(); }), "grace publishes admission closure immediately");
    Check(stop.WaitFor(0ms) == std::future_status::timeout, "grace waits for begun request");
    CheckEq(server.Get("/during", [](auto&, auto&) {}).error(), mhttp::Error::AlreadyRunning, "routing remains frozen until full drain");
    finish.store(true);
    auto response = request.Get();
    Check(stop.Get().has_value(), "natural completion reports successful grace");
    Check(response && response->Body() == "completed" && !response->KeepAlive(), "begun response completes and closes persistence");
    Check(!server.LocalEndpoint() && server.Get("/new", [](auto&, auto&) {}).has_value(), "complete drain thaws routes");
    auto next_port = start_grace_server(server);
    mhttp::client::HttpClient next{ runtime, "127.0.0.1", next_port };
    auto restarted = mhttp::SyncWait(runtime, next.Get("/new"));
    Check(restarted && restarted->Status() == mhttp::Status::Ok, "natural grace permits listener restart");
}

/**
 * @brief Await grace with a single I/O thread while independent control work continues to execute.
 */
auto nonblocking_async_grace() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(1).has_value(), "start one-thread async grace runtime");
    std::binary_semaphore entered{ 0 };
    std::atomic<bool> finish{};
    mhttp::server::HttpServer server{ runtime };
    Check(server.Get("/", [&](auto&, auto& response) -> mhttp::Task<void> {
        entered.release();
        while (!finish.load()) { co_await mhttp::Sleep(1ms); }
        static_cast<void>(response.SetContent("async", "text/plain"));
    }).has_value(), "register async grace response");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_grace_server(server) };
    mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(runtime, client.Get()); } };
    Check(entered.try_acquire_for(1s), "async request begins before grace");
    mhttp::test::ThreadTask stop{ [&] { return mhttp::SyncWait(runtime, server.AsyncStop(2s)); } };
    Check(grace_eventually([&] { return !server.IsRunning(); }), "async grace begins");
    mhttp::SyncWait(runtime, mhttp::Sleep(1ms));
    Check(stop.WaitFor(0ms) == std::future_status::timeout, "independent Task runs while shutdown awaits the active request");
    finish.store(true);
    auto response = request.Get();
    Check(stop.Get().has_value() && response && response->Body() == "async", "async grace completes on one I/O thread");
}

/**
 * @brief Cancel and clean up a slow request at synchronous, asynchronous, and zero grace deadlines.
 */
auto grace_deadline() -> void {
    for (unsigned mode{}; mode < 3; ++mode) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start forced grace runtime");
        std::binary_semaphore entered{ 0 };
        std::atomic<unsigned> active{};
        mhttp::server::HttpServer server{ runtime };
        Check(server.Get("/", [&](auto&, auto&) -> mhttp::Task<void> {
            GraceFrame frame{ active };
            entered.release();
            co_await mhttp::Sleep(5s);
        }).has_value(), "register deadline-bound handler");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_grace_server(server) };
        mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(runtime, client.Get()); } };
        Check(entered.try_acquire_for(1s), "slow request begins before grace deadline");
        auto const started = std::chrono::steady_clock::now();
        auto stopped = mode == 1 ? mhttp::SyncWait(runtime, server.AsyncStop(40ms)) : server.Stop(mode == 2 ? 0ms : 40ms);
        auto response = request.Get();
        Check(!stopped && stopped.error() == mhttp::Error::ShutdownTimeout, "deadline reports forced shutdown after cleanup");
        Check(!response && active == 0 && !server.LocalEndpoint(), "deadline cancels response and drains its handler frame");
        Check(std::chrono::steady_clock::now() - started < 1s, "deadline does not wait for slow handler delay");
        Check(server.Stop().has_value(), "immediate Stop remains idempotent after deadline drain");
    }
}

/**
 * @brief Merge concurrent graceful requests using the earliest absolute deadline, without extending the budget.
 */
auto concurrent_grace_deadlines() -> void {
    for (bool shorten : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start concurrent grace runtime");
        std::binary_semaphore entered{ 0 };
        mhttp::server::HttpServer server{ runtime };
        Check(server.Get("/", [&](auto&, auto&) -> mhttp::Task<void> { entered.release(); co_await mhttp::Sleep(5s); }).has_value(), "register concurrent grace handler");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_grace_server(server) };
        mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(runtime, client.Get()); } };
        Check(entered.try_acquire_for(1s), "concurrent grace request entered");
        auto const started = std::chrono::steady_clock::now();
        mhttp::test::ThreadTask first{ [&] { return server.Stop(shorten ? 2s : 80ms); } };
        Check(grace_eventually([&] { return !server.IsRunning(); }), "first graceful caller closes admission");
        auto second = mhttp::SyncWait(runtime, server.AsyncStop(shorten ? 40ms : 2s));
        auto original = first.Get();
        Check(!original && original.error() == mhttp::Error::ShutdownTimeout && !second && second.error() == mhttp::Error::ShutdownTimeout, "concurrent callers share forced drain outcome");
        Check(std::chrono::steady_clock::now() - started < 1s, "later requests never extend earliest grace");
        Check(!request.Get(), "concurrent grace closes unfinished response");
    }
}

/**
 * @brief Escalate grace immediately through existing Stop or Runtime shutdown and report cancellation.
 */
auto immediate_escalation() -> void {
    for (bool stop_runtime : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start escalation runtime");
        std::binary_semaphore entered{ 0 };
        std::atomic<unsigned> active{};
        mhttp::server::HttpServer server{ runtime };
        Check(server.Get("/", [&](auto&, auto&) -> mhttp::Task<void> {
            GraceFrame frame{ active }; entered.release(); co_await mhttp::Sleep(5s);
        }).has_value(), "register escalation handler");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_grace_server(server) };
        mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(runtime, client.Get()); } };
        Check(entered.try_acquire_for(1s), "escalation handler begins");
        mhttp::test::ThreadTask stop{ [&] { return server.Stop(2s); } };
        Check(grace_eventually([&] { return !server.IsRunning(); }), "grace begins before escalation");
        if (stop_runtime) { runtime.Stop(); }
        else { Check(server.Stop().has_value(), "immediate Stop supersedes grace"); }
        auto result = stop.Get();
        Check(!result && result.error() == mhttp::Error::Canceled && active == 0, "escalation cancels and joins graceful requests");
        Check(!request.Get(), "escalation returns no unfinished HTTP response");
        if (stop_runtime) { Check(runtime.Join().has_value(), "runtime escalation drains timer and control callbacks"); }
    }
}

/**
 * @brief Initiate cancellation at the deadline but wait for an already executing worker before publishing completion.
 */
auto offload_cleanup() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start worker grace runtime");
    std::binary_semaphore entered{ 0 }, release{ 0 };
    std::atomic<unsigned> active{};
    mhttp::server::HttpServer server{ runtime };
    Check(server.Get("/", [&](auto&, auto&) -> mhttp::Task<void> {
        GraceFrame frame{ active };
        co_await mhttp::Offload([&] { entered.release(); release.acquire(); });
    }).has_value(), "register owned blocking worker");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_grace_server(server) };
    mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(runtime, client.Get()); } };
    Check(entered.try_acquire_for(1s), "Offload worker starts before grace");
    mhttp::test::ThreadTask stop{ [&] { return server.Stop(40ms); } };
    Check(request.WaitFor(1s) == std::future_status::ready, "deadline closes socket while worker remains running");
    Check(stop.WaitFor(0ms) == std::future_status::timeout && active == 1, "Stop retains worker-borrowed state until cleanup");
    release.release();
    auto result = stop.Get();
    Check(!result && result.error() == mhttp::Error::ShutdownTimeout && active == 0, "deadline outcome published after worker and handler frame drain");
    Check(!request.Get(), "canceled worker produces no partial response");
}

/**
 * @brief Cancel a graceful stop from an independent calling session, forcing and joining its target generation.
 */
auto caller_cancellation() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start control-session grace runtime");
    std::binary_semaphore entered{ 0 };
    std::atomic<unsigned> active{};
    std::atomic<bool> canceled{};
    mhttp::server::HttpServer target{ runtime };
    Check(target.Get("/", [&](auto&, auto&) -> mhttp::Task<void> {
        GraceFrame frame{ active }; entered.release(); co_await mhttp::Sleep(5s);
    }).has_value(), "register control cancellation target");
    mhttp::client::HttpClient upstream{ runtime, "127.0.0.1", start_grace_server(target) };
    mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(runtime, upstream.Get()); } };
    Check(entered.try_acquire_for(1s), "target request enters before control grace");
    mhttp::server::HttpServer control{ runtime };
    Check(control.Get("/", [&](auto&, auto&) -> mhttp::Task<void> {
        auto result = co_await target.AsyncStop(5s);
        canceled.store(!result && result.error() == mhttp::Error::Canceled);
    }).has_value(), "register independent asynchronous shutdown controller");
    mhttp::client::HttpClient caller{ runtime, "127.0.0.1", start_grace_server(control) };
    mhttp::test::ThreadTask command{ [&] { return mhttp::SyncWait(runtime, caller.Get()); } };
    Check(grace_eventually([&] { return !target.IsRunning(); }), "control session starts target grace");
    Check(control.Stop().has_value(), "cancel independent control session");
    Check(!command.Get() && !request.Get() && canceled && active == 0, "caller cancellation forces and joins target cleanup");
    Check(!target.LocalEndpoint(), "canceled graceful control leaves target fully stopped");
}

/**
 * @brief Stop a Listen-owned inline Runtime from a separate caller Runtime without losing queued drain completions.
 */
auto owned_listen_runtime() -> void {
    mhttp::Runtime target_runtime;
    mhttp::Runtime caller_runtime;
    Check(caller_runtime.Start(2).has_value(), "start separate Listen controller runtime");
    std::binary_semaphore entered{ 0 };
    mhttp::server::HttpServer server{ target_runtime };
    Check(server.Get("/", [&](auto&, auto& response) -> mhttp::Task<void> {
        entered.release(); co_await mhttp::Sleep(40ms);
        static_cast<void>(response.SetContent("inline", "text/plain"));
    }).has_value(), "register inline grace response");
    mhttp::test::ThreadTask listener{ [&] { return server.Listen("127.0.0.1", 0); } };
    Check(grace_eventually([&] { return server.IsRunning(); }) && server.WaitUntilReady(), "Listen owns running inline generation");
    mhttp::client::HttpClient client{ caller_runtime, "127.0.0.1", server.LocalEndpoint()->port };
    mhttp::test::ThreadTask request{ [&] { return mhttp::SyncWait(caller_runtime, client.Get()); } };
    Check(entered.try_acquire_for(1s), "inline handler starts before asynchronous grace");
    auto stopped = mhttp::SyncWait(caller_runtime, server.AsyncStop(2s));
    auto response = request.Get();
    Check(stopped && response && response->Body() == "inline" && listener.Get(), "cross-runtime graceful stop drains inline Listen");
    Check(!target_runtime.IsRunning(), "completed grace stops only the Listen-owned Runtime");
}

/**
 * @brief Run public graceful lifecycle cases with per-case watchdogs.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "grace configuration and ownership", grace_configuration }, { "natural graceful completion", graceful_completion },
        { "nonblocking asynchronous grace", nonblocking_async_grace }, { "grace deadline cancellation", grace_deadline },
        { "earliest concurrent grace deadline", concurrent_grace_deadlines }, { "immediate grace escalation", immediate_escalation },
        { "Offload grace cleanup", offload_cleanup }, { "calling session cancellation", caller_cancellation },
        { "Listen-owned inline grace", owned_listen_runtime },
    });
}
