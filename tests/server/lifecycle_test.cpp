/**
 * @file lifecycle_test.cpp
 * @brief Verify public server binding, readiness, restart, and synchronous/asynchronous drain.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Verify idle/bound states without starting a runtime and rollback after Start rejection.
 */
auto server_bound_lifecycle() -> void {
    mhttp::Runtime runtime;
    mhttp::server::HttpServer server{ runtime };
    Check(!server.IsRunning() && !server.LocalEndpoint(), "new server has no listener");
    CheckEq(server.Start().error(), mhttp::Error::NotRunning, "unbound server cannot start");
    CheckEq(server.WaitUntilReady().error(), mhttp::Error::NotRunning, "idle readiness never blocks");
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!Check(port.has_value(), "bind without starting runtime")) { return; }
    Check(*port != 0, "OS allocates a nonzero port");
    CheckEq(server.LocalEndpoint(), std::optional{ mhttp::Endpoint{ "127.0.0.1", *port } }, "owned local snapshot");
    CheckEq(server.WaitUntilReady().error(), mhttp::Error::NotRunning, "bound socket is not a submitted server");
    CheckEq(server.Start().error(), mhttp::Error::NotRunning, "Start requires a running runtime");
    Check(server.Get("/", [](auto&, auto&) {}).has_value(), "failed Start rolls back router freeze");
    mhttp::server::HttpServer conflicting{ runtime };
    CheckEq(conflicting.Bind("127.0.0.1", *port).error(), mhttp::Error::BindIpAddress,
        "second listener cannot bind an occupied loopback port");
    Check(!conflicting.LocalEndpoint(), "failed bind publishes no endpoint");
    Check(server.Stop().has_value(), "stop a socket that was only bound");
    Check(!server.LocalEndpoint(), "stop clears listener snapshot");
    Check(server.Stop().has_value(), "repeated Stop is safe");
    Check(conflicting.Bind("127.0.0.1", *port).has_value(), "bound-only Stop releases the actual port");
}

/**
 * @brief Verify Start/Stop races, generation-specific readiness, and retained route configuration.
 */
auto server_quick_restart() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start multithreaded server runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Get("/", [](auto&, auto&) {}).has_value(), "register retained route");
    for (unsigned iteration{}; iteration < 100; ++iteration) {
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind a fresh generation");
        Check(server.Start().has_value(), "start a fresh generation");
        Check(server.IsRunning(), "Start publishes running before return");
        CheckEq(server.Start().error(), mhttp::Error::AlreadyRunning, "duplicate Start is rejected");
        CheckEq(server.Get("/during", [](auto&, auto&) {}).error(), mhttp::Error::AlreadyRunning,
            "runtime route registration is guarded");
        if (iteration % 2 == 0) { Check(server.WaitUntilReady().has_value(), "wait for submitted generation"); }
        Check(server.Stop().has_value(), "stop immediately or after readiness");
        Check(!server.IsRunning() && !server.LocalEndpoint(), "drain clears active generation");
        CheckEq(server.WaitUntilReady().error(), mhttp::Error::NotRunning, "stopped readiness returns immediately");
        Check(server.Get("/after", [](auto&, auto&) {}).has_value(), "drain thaws route registration");
    }
}

/**
 * @brief Verify concurrent non-I/O Stop callers join the same generation safely.
 */
auto server_concurrent_stop() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start concurrent-stop runtime");
    mhttp::server::HttpServer server{ runtime };
    for (unsigned iteration{}; iteration < 20; ++iteration) {
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind concurrent-stop generation");
        Check(server.Start().has_value(), "start concurrent-stop generation");
        std::vector<std::jthread> stoppers;
        for (unsigned index{}; index < 4; ++index) {
            stoppers.emplace_back([&] { Check(server.Stop().has_value(), "concurrent Stop completes"); });
        }
        stoppers.clear();
        Check(!server.IsRunning(), "all Stop callers observe drain");
    }
}

/**
 * @brief Request asynchronous drain from a Task while checking blocking entry points reject I/O callers.
 * @param server Borrowed server, alive until SyncWait returns after root destruction.
 */
auto stop_server_task(mhttp::server::HttpServer& server) -> mhttp::Task<void> {
    CheckEq(server.Stop().error(), mhttp::Error::Busy, "blocking Stop rejects I/O threads");
    CheckEq(server.WaitUntilReady().error(), mhttp::Error::Busy, "blocking readiness rejects I/O threads");
    CheckEq(server.BindToAnyPort("127.0.0.1").error(), mhttp::Error::Busy, "blocking Bind rejects I/O threads");
    CheckEq(server.Listen("127.0.0.1", 0).error(), mhttp::Error::Busy, "blocking Listen rejects I/O threads");
    co_await server.AsyncStop();
    Check(!server.IsRunning() && !server.LocalEndpoint(), "AsyncStop returns only after listener drain");
    Check(server.Get("/after-async-stop", [](auto&, auto&) {}).has_value(), "AsyncStop thaws routing before return");
}

/**
 * @brief Verify asynchronous stop and Runtime::Stop cancellation across fresh generations.
 */
auto server_async_and_runtime_stop() -> void {
    mhttp::Runtime runtime;
    mhttp::server::HttpServer server{ runtime };
    for (unsigned iteration{}; iteration < 10; ++iteration) {
        Check(runtime.Start(4).has_value(), "start runtime generation");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind runtime generation");
        Check(server.Start().has_value(), "submit listener");
        if (iteration % 2 == 0) { mhttp::SyncWait(runtime, stop_server_task(server)); }
        runtime.Stop();
        Check(runtime.Join().has_value(), "runtime stop cancels accept instead of waiting forever");
        Check(server.Stop().has_value(), "server Stop after runtime drain returns immediately");
        Check(!server.IsRunning(), "runtime cancellation finishes server generation");
    }
}

/**
 * @brief Reject invalid options before binding and automatically release active listeners on destruction.
 */
auto server_configuration_and_destruction() -> void {
    mhttp::Runtime runtime;
    mhttp::server::HttpServer invalid{ runtime, { .read_timeout = 0ms } };
    CheckEq(invalid.BindToAnyPort("127.0.0.1").error(), mhttp::Error::InvalidRequest, "invalid options never bind");
    Check(runtime.Start(4).has_value(), "start destruction runtime");
    std::uint16_t port{};
    {
        mhttp::server::HttpServer server{ runtime };
        auto bound = server.BindToAnyPort("127.0.0.1");
        if (!Check(bound.has_value(), "bind destructed server")) { return; }
        port = *bound;
        Check(server.Start().has_value(), "start destructed server");
        Check(server.WaitUntilReady().has_value(), "destructed server becomes ready");
    }
    mhttp::server::HttpServer replacement{ runtime };
    Check(replacement.Bind("127.0.0.1", port).has_value(), "destructor drains and releases the listening port");
}

/**
 * @brief Verify Listen can own inline execution or wait on an already running shared Runtime.
 */
auto server_blocking_listen() -> void {
    for (bool started_runtime : { false, true }) {
        mhttp::Runtime runtime;
        if (started_runtime) { Check(runtime.Start(4).has_value(), "start shared Listen runtime"); }
        mhttp::server::HttpServer server{ runtime };
        for (unsigned iteration{}; iteration < 10; ++iteration) {
            std::optional<mhttp::Result<void>> result;
            std::jthread listener{ [&] { result = server.Listen("127.0.0.1", 0); } };
            while (!server.IsRunning()) { std::this_thread::sleep_for(1ms); }
            Check(server.WaitUntilReady().has_value(), "blocking Listen publishes ready state");
            Check(server.LocalEndpoint() && server.LocalEndpoint()->port != 0, "Listen dynamic endpoint visible");
            Check(server.Stop().has_value(), "external Stop releases Listen");
            listener.join();
            Check(result && result->has_value(), "Listen returns success after drain");
            CheckEq(runtime.IsRunning(), started_runtime, "Listen stops only its own inline execution generation");
            Check(server.Get("/after-listen", [](auto&, auto&) {}).has_value(), "Listen permits reuse after drain");
        }
    }
}

/**
 * @brief Run public server lifecycle scenarios with bounded per-case watchdogs.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Process test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "server bound lifecycle", server_bound_lifecycle },
        { "server quick restart", server_quick_restart },
        { "server concurrent Stop", server_concurrent_stop },
        { "server asynchronous and runtime Stop", server_async_and_runtime_stop },
        { "server configuration and destruction", server_configuration_and_destruction },
        { "server blocking Listen", server_blocking_listen },
    });
}
