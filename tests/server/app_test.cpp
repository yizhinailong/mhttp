/**
 * @file app_test.cpp
 * @brief Verify the process-wide application entry: configuration, serving, quit paths, restart, and signals.
 */
#include <csignal>

import std;
import mhttp;

#include "../support/test_support.hpp"
#include "../support/thread_task.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Send one GET to a loopback listener from an independent client runtime.
 * @param port Listening port.
 * @param target Origin-form request target.
 * @return Response body, or nullopt when the exchange failed.
 */
auto fetch(std::uint16_t port, std::string target) -> std::optional<std::string> {
    mhttp::Runtime runtime;
    if (!runtime.Start()) { return std::nullopt; }
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    auto response = mhttp::SyncWait(runtime, client.Get(std::move(target)));
    if (!response) { return std::nullopt; }
    return response->Body();
}

/**
 * @brief Verify blocking application entry points reject I/O callers.
 * @param app Borrowed idle application, alive until SyncWait returns.
 */
auto reject_io_callers(mhttp::App& app) -> mhttp::Task<void> {
    CheckEq(app.Run().error(), mhttp::Error::Busy, "Run rejects I/O threads");
    CheckEq(app.WaitUntilReady().error(), mhttp::Error::Busy, "readiness wait rejects I/O threads");
    co_return;
}

/**
 * @brief Report invalid settings and bind failures from Run without publishing a listener.
 */
auto app_configuration() -> void {
    std::vector<mhttp::server::LogEvent> events;
    mhttp::App app;
    app.DisableSignalHandling();
    CheckEq(app.Run().error(), mhttp::Error::InvalidRequest, "Run requires a listener");
    app.SetListener("127.0.0.1", 0).SetServerOptions({ .read_timeout = 0ms });
    CheckEq(app.Run().error(), mhttp::Error::InvalidRequest, "invalid server options fail before serving");
    app.SetServerOptions({}).SetShutdownTimeout(-1ms);
    CheckEq(app.Run().error(), mhttp::Error::InvalidRequest, "negative shutdown timeout is rejected");
    app.SetShutdownTimeout(1s);
    mhttp::Runtime runtime;
    mhttp::server::HttpServer occupant{ runtime };
    auto port = occupant.BindToAnyPort("127.0.0.1");
    if (!Check(port.has_value(), "occupy a loopback port")) { return; }
    app.SetListener("127.0.0.1", *port).SetLogger([&events](mhttp::server::LogRecord const& record) {
        events.push_back(record.event);
    });
    CheckEq(app.Run().error(), mhttp::Error::BindIpAddress, "occupied port fails Run");
    CheckEq(events, std::vector{ mhttp::server::LogEvent::BindFailure }, "logger is forwarded to the listener");
    Check(!app.IsRunning() && !app.LocalEndpoint(), "failed Run publishes no listener");
    CheckEq(app.WaitUntilReady(0ms).error(), mhttp::Error::ConnectionTimeout, "readiness needs an active Run");
    Check(app.set_http_handler<mhttp::method::Get>("/", [](auto&, auto&) {}).has_value(),
        "failed Run leaves routes mutable");
    mhttp::Runtime caller;
    Check(caller.Start().has_value(), "start I/O caller runtime");
    mhttp::SyncWait(caller, reject_io_callers(app));
}

/**
 * @brief Serve registered routes, freeze configuration while running, and run again after Quit.
 */
auto app_serve_and_restart() -> void {
    std::atomic<unsigned> logged{};
    mhttp::App app;
    Check(app.set_http_handler<mhttp::method::Get>("/hello", [](auto&, auto& response) {
        Check(response.SetContent("hello", "text/plain").has_value(), "hello response");
    }).has_value(), "register before Run");
    app.SetListener("127.0.0.1", 0).SetThreadNum(2).DisableSignalHandling()
        .SetAccessLogger([&logged](auto const&, auto const&, auto) { ++logged; });
    mhttp::test::ThreadTask first{ [&app] { return app.Run(); } };
    if (!Check(app.WaitUntilReady().has_value(), "Run publishes a ready listener")) { app.Quit(); return; }
    Check(app.IsRunning(), "ready application reports running");
    auto endpoint = app.LocalEndpoint();
    if (!Check(endpoint && endpoint->port != 0, "dynamic port is published")) { app.Quit(); return; }
    CheckEq(fetch(endpoint->port, "/hello"), std::optional<std::string>{ "hello" }, "listener serves registered route");
    CheckEq(app.Run().error(), mhttp::Error::AlreadyRunning, "concurrent Run is rejected");
    CheckEq(app.set_http_handler<mhttp::method::Get>("/later", [](auto&, auto&) {}).error(),
        mhttp::Error::AlreadyRunning, "routes are frozen while running");
    app.Quit();
    Check(first.Get().has_value(), "Quit ends Run successfully");
    Check(!app.IsRunning() && !app.LocalEndpoint(), "returned Run clears the listener");
    CheckEq(logged.load(), 1u, "access logger observes the request");

    Check(app.set_http_handler<mhttp::method::Get>("/later", [](auto&, auto& response) {
        Check(response.SetContent("later", "text/plain").has_value(), "later response");
    }).has_value(), "routes thaw after Run");
    app.SetThreadNum(0);
    mhttp::test::ThreadTask second{ [&app] { return app.Run(); } };
    if (!Check(app.WaitUntilReady().has_value(), "second Run becomes ready")) { app.Quit(); return; }
    endpoint = app.LocalEndpoint();
    if (!Check(endpoint.has_value(), "second Run publishes its port")) { app.Quit(); return; }
    CheckEq(fetch(endpoint->port, "/later"), std::optional<std::string>{ "later" }, "restarted Run serves new route");
    app.Quit();
    Check(second.Get().has_value(), "second Run quits");
}

/**
 * @brief Let a handler quit its own application while its begun response still completes.
 */
auto app_quit_from_handler() -> void {
    mhttp::App app;
    Check(app.set_http_handler<mhttp::method::Get>("/quit", [&app](auto&, auto& response) {
        app.Quit();
        Check(response.SetContent("bye", "text/plain").has_value(), "quit response");
    }).has_value(), "register quitting route");
    app.SetListener("127.0.0.1", 0).DisableSignalHandling();
    mhttp::test::ThreadTask run{ [&app] { return app.Run(); } };
    if (!Check(app.WaitUntilReady().has_value(), "quit fixture becomes ready")) { app.Quit(); return; }
    auto endpoint = app.LocalEndpoint();
    if (!Check(endpoint.has_value(), "quit fixture publishes its port")) { app.Quit(); return; }
    CheckEq(fetch(endpoint->port, "/quit"), std::optional<std::string>{ "bye" }, "begun request completes after Quit");
    Check(run.Get().has_value(), "handler Quit ends Run");
}

/**
 * @brief Cancel a begun request once the shutdown budget expires and report the forced drain.
 */
auto app_shutdown_timeout() -> void {
    std::binary_semaphore entered{ 0 };
    mhttp::App app;
    Check(app.set_http_handler<mhttp::method::Get>("/slow", [&entered](auto&, auto&) -> mhttp::Task<void> {
        entered.release();
        co_await mhttp::Sleep(10s);
    }).has_value(), "register slow route");
    app.SetListener("127.0.0.1", 0).SetShutdownTimeout(50ms).DisableSignalHandling();
    mhttp::test::ThreadTask run{ [&app] { return app.Run(); } };
    if (!Check(app.WaitUntilReady().has_value(), "timeout fixture becomes ready")) { app.Quit(); return; }
    auto endpoint = app.LocalEndpoint();
    if (!Check(endpoint.has_value(), "timeout fixture publishes its port")) { app.Quit(); return; }
    mhttp::test::ThreadTask request{ [port = endpoint->port] { return fetch(port, "/slow"); } };
    if (!Check(entered.try_acquire_for(2s), "slow request begins")) { app.Quit(); return; }
    auto const quit = std::chrono::steady_clock::now();
    app.Quit();
    auto result = run.Get();
    Check(!result && result.error() == mhttp::Error::ShutdownTimeout, "expired budget reports forced drain");
    Check(std::chrono::steady_clock::now() - quit < 2s, "Run returns soon after the budget expires");
    Check(!request.Get(), "canceled request fails at the client");
}

/**
 * @brief Quit the process-wide application through SIGINT and restore default handling.
 */
auto app_signal_quit() -> void {
    auto& app = mhttp::app();
    Check(&app == &mhttp::app(), "app returns one process-wide instance");
    app.SetListener("127.0.0.1", 0);
    mhttp::test::ThreadTask run{ [&app] { return app.Run(); } };
    if (!Check(app.WaitUntilReady().has_value(), "singleton becomes ready")) { app.Quit(); return; }
    Check(std::raise(SIGINT) == 0, "raise SIGINT");
    Check(run.Get().has_value(), "SIGINT quits Run gracefully");
}

auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "app configuration failures", app_configuration }, { "app serve and restart", app_serve_and_restart },
        { "app quit from handler", app_quit_from_handler }, { "app shutdown timeout", app_shutdown_timeout },
        { "app signal quit", app_signal_quit },
    });
}
