/**
 * @file keep_alive_test.cpp
 * @brief Verify idle close detection, persistent reuse, request-count reconnects, and idle-owner cleanup.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Start one echo endpoint returning the server-observed client port.
 * @param server Idle server on a running runtime, retained through all client requests.
 * @return Dynamic loopback listening port.
 */
auto start_keep_alive_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    Check(server.Get("/", [](auto& request, auto& response) {
        Check(response.Headers().Set("X-Peer-Port", std::to_string(request.RemoteEndpoint().port)).has_value(), "return observed peer port");
        Check(response.SetContent("ready", "text/plain").has_value(), "persistent echo content");
    }).has_value(), "register persistence route");
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start keep-alive server" }; }
    return *port;
}

/**
 * @brief Observe an idle server FIN before the next header write and reconnect without exposing failure.
 */
auto idle_server_close() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start idle-close runtime");
    mhttp::server::HttpServer server{ runtime, { .keep_alive_timeout = 50ms } };
    auto port = start_keep_alive_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    auto first = mhttp::SyncWait(runtime, client.Get());
    if (!Check(first.has_value(), "first idle-close request succeeds")) { return; }
    auto const until = std::chrono::steady_clock::now() + 1s;
    while (client.IsOpen() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(5ms); }
    Check(!client.IsOpen(), "idle monitor observes server closure without sending another request");
    auto next = mhttp::SyncWait(runtime, client.Get());
    Check(next && next->Body() == "ready", "request after idle close reconnects transparently");
    if (next) { Check(first->LocalEndpoint() != next->LocalEndpoint(), "idle-close successor uses a fresh TCP socket"); }
}

/**
 * @brief Repeatedly retire idle waits without disturbing a usable persistent connection.
 */
auto repeated_persistent_requests() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start repeated persistence runtime");
    mhttp::server::HttpServer server{ runtime, { .keep_alive_max_requests = 1000 } };
    auto port = start_keep_alive_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    std::optional<mhttp::Endpoint> local;
    for (unsigned i{}; i < 200; ++i) {
        auto response = mhttp::SyncWait(runtime, client.Get());
        if (!Check(response.has_value(), "persistent request survives idle-wait cancellation")) { continue; }
        if (local) { CheckEq(response->LocalEndpoint(), *local, "two hundred requests retain the same socket"); }
        else { local = response->LocalEndpoint(); }
        CheckEq(response->Header("X-Peer-Port"), std::to_string(response->LocalEndpoint().port), "server sees the reused client socket");
        Check(response->KeepAlive(), "idle monitoring does not alter negotiated persistence");
    }
}

/**
 * @brief Reconnect after every server-imposed maximum-request close without duplicating or losing requests.
 */
auto request_count_reconnect() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start request-count runtime");
    mhttp::server::HttpServer server{ runtime, { .keep_alive_max_requests = 3 } };
    auto port = start_keep_alive_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    std::optional<mhttp::Endpoint> previous;
    for (unsigned i{}; i < 10; ++i) {
        auto response = mhttp::SyncWait(runtime, client.Get());
        if (!Check(response && response->Body() == "ready", "request count closure is transparent to callers")) { continue; }
        if (previous) {
            Check((response->LocalEndpoint() != *previous) == (i % 3 == 0), "only a server-imposed boundary changes the socket");
        }
        previous = response->LocalEndpoint();
        CheckEq(response->KeepAlive(), i % 3 != 2, "last response in each generation advertises closure");
    }
}

/**
 * @brief Disable persistence explicitly and establish a fresh connection for every request.
 */
auto disabled_persistence() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start nonpersistent runtime");
    mhttp::server::HttpServer server{ runtime };
    auto port = start_keep_alive_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port, { .keep_alive = false } };
    std::optional<mhttp::Endpoint> previous;
    for (unsigned i{}; i < 3; ++i) {
        auto response = mhttp::SyncWait(runtime, client.Get());
        if (!Check(response.has_value(), "nonpersistent request succeeds")) { continue; }
        Check(!response->KeepAlive() && !client.IsOpen(), "configuration closes every completed connection");
        if (previous) { Check(response->LocalEndpoint() != *previous, "disabled persistence always creates a new socket"); }
        previous = response->LocalEndpoint();
    }
}

/**
 * @brief Cancel issue-time epochs while an idle watch exists and keep stale stop posts from closing new requests.
 */
auto idle_stop_epochs() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start idle epoch runtime");
    mhttp::server::HttpServer server{ runtime };
    auto port = start_keep_alive_server(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    for (unsigned i{}; i < 25; ++i) {
        auto first = mhttp::SyncWait(runtime, client.Get());
        auto issued = client.Get();
        client.Stop();
        auto fresh = client.Get();
        auto canceled = mhttp::SyncWait(runtime, std::move(issued));
        Check(!canceled && canceled.error() == mhttp::Error::Canceled, "Stop cancels an issued but unstarted request");
        auto next = mhttp::SyncWait(runtime, std::move(fresh));
        Check(first && next && next->Body() == "ready", "new epoch remains usable after the stop post and old watch drain");
        if (first && next) { Check(first->LocalEndpoint() != next->LocalEndpoint(), "explicit Stop retires the old monitored socket"); }
    }
}

/**
 * @brief Destroy clients with pending idle waits and prove their socket work cannot delay runtime drain.
 */
auto idle_client_destruction() -> void {
    mhttp::Runtime server_runtime;
    Check(server_runtime.Start(4).has_value(), "start idle destruction server runtime");
    mhttp::server::HttpServer server{ server_runtime };
    auto port = start_keep_alive_server(server);
    for (unsigned i{}; i < 15; ++i) {
        mhttp::Runtime runtime;
        Check(runtime.Start(3).has_value(), "start disposable client runtime");
        {
            mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
            auto response = mhttp::SyncWait(runtime, client.Get());
            Check(response && response->Body() == "ready", "disposable client establishes a monitored idle socket");
        }
        auto const started = std::chrono::steady_clock::now();
        runtime.Stop();
        Check(runtime.Join().has_value(), "destroyed idle client drains every monitor completion");
        Check(std::chrono::steady_clock::now() - started < 1s, "client destruction does not wait for the server idle timeout");
    }
}

/**
 * @brief Run bounded persistent-client acceptance cases.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Test runner status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client idle server close", idle_server_close },
        { "client repeated persistent requests", repeated_persistent_requests },
        { "client request count reconnect", request_count_reconnect },
        { "client disabled persistence", disabled_persistence },
        { "client idle Stop epochs", idle_stop_epochs },
        { "client idle destruction", idle_client_destruction },
    });
}
