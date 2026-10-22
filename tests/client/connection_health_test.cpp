/**
 * @file connection_health_test.cpp
 * @brief Exercise nonconsuming asynchronous connection probes against live and closed loopback peers.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"
#include "../support/thread_task.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Start a configured server on a dynamic loopback port.
 * @param server Server retained through all operations.
 * @return Bound port.
 */
auto start_health_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "health fixture start failed" }; }
    return *port;
}

/**
 * @brief Leave lazy clients disconnected and preserve a healthy persistent socket through repeated probes.
 */
auto quiet_connections() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start health runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Get>("/", [](auto&, auto& response) { Check(response.SetContent("ready", "text/plain").has_value(), "health response"); }).has_value(), "register health endpoint");
    auto port = start_health_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    auto lazy = mhttp::SyncWait(runtime, client.IsAlive());
    Check(lazy && !*lazy && !client.IsOpen(), "probe does not establish a lazy connection");
    auto initial = mhttp::SyncWait(runtime, client.Get());
    if (!Check(initial.has_value(), "establish probe socket")) { return; }
    for (unsigned index{}; index < 20; ++index) {
        auto alive = mhttp::SyncWait(runtime, client.IsAlive());
        Check(alive && *alive && client.IsOpen(), "quiet readiness timeout preserves connection");
        auto response = mhttp::SyncWait(runtime, client.Get());
        Check(response && response->Body() == "ready" && response->LocalEndpoint() == initial->LocalEndpoint(), "probe consumes no bytes and retires its receive guard");
    }
    auto issued = client.IsAlive();
    client.Stop();
    auto canceled = mhttp::SyncWait(runtime, std::move(issued));
    Check(!canceled && canceled.error() == mhttp::Error::Canceled, "Stop invalidates an issued lazy probe");
    auto closed = mhttp::SyncWait(runtime, client.IsAlive());
    Check(closed && !*closed, "new probe after Stop does not inspect the retired socket");
}

/**
 * @brief Report an observed peer FIN without connecting again, permitting a later Send to reconnect.
 */
auto peer_closure() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start peer-close health runtime");
    mhttp::server::HttpServer server{ runtime, { .keep_alive_timeout = 30ms } };
    Check(server.set_http_handler<mhttp::method::Get>("/", [](auto&, auto& response) { Check(response.SetContent("ready", "text/plain").has_value(), "peer-close content"); }).has_value(), "register peer-close health endpoint");
    auto port = start_health_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    auto first = mhttp::SyncWait(runtime, client.Get());
    std::this_thread::sleep_for(80ms);
    auto dead = mhttp::SyncWait(runtime, client.IsAlive());
    Check(dead && !*dead && !client.IsOpen(), "closed idle socket is not healthy");
    auto second = mhttp::SyncWait(runtime, client.Get());
    Check(first && second && second->Body() == "ready" && first->LocalEndpoint() != second->LocalEndpoint(), "later request reconnects after observed FIN");
}

/**
 * @brief Reject a probe overlapping a pending response without interrupting the transfer's cancellation path.
 */
auto busy_connection() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start busy-probe runtime");
    std::binary_semaphore entered{ 0 };
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Get>("/", [&entered](auto&, auto&) -> mhttp::Task<void> {
        entered.release();
        co_await mhttp::Sleep(5s);
    }).has_value(), "register pending health endpoint");
    auto port = start_health_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    mhttp::test::ThreadTask pending{ [&] { return mhttp::SyncWait(runtime, client.Get()); } };
    Check(entered.try_acquire_for(1s), "request begins before overlapping probe");
    auto busy = mhttp::SyncWait(runtime, client.IsAlive());
    Check(!busy && busy.error() == mhttp::Error::Busy, "probe cannot overlap single-flight Send");
    client.Stop();
    auto canceled = pending.Get();
    Check(!canceled && canceled.error() == mhttp::Error::Canceled, "overlapping probe preserves pending request cancellation");
}

/**
 * @brief Run health probe cases.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "quiet persistent health", quiet_connections }, { "observed peer closure", peer_closure },
        { "busy connection probe", busy_connection },
    });
}
