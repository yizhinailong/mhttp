/**
 * @file client_ip_test.cpp
 * @brief Verify trusted client IPs through public listeners, async handlers, logs, and concurrent sessions.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Observe the resolved address on both sides of the awaited handler without retaining views.
 */
struct ClientIpAspect {
    /**
     * @brief Copy the resolved IP into request-local state before the handler.
     * @param request Borrowed request with already computed client metadata.
     * @param response Unused mutable response.
     * @return True to continue routing.
     */
    auto Before(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        CheckEq(request.Data().Fetch<std::string>("pre_ip"), std::optional{ request.ClientIp() },
            "pre-routing and aspect see the same IP");
        request.Data().Store("aspect_ip", request.ClientIp());
        return true;
    }

    /**
     * @brief Verify the same owned address after the asynchronous handler completes.
     * @param request Borrowed request with handler-populated response.
     * @param response Borrowed final content, equal to the resolved address.
     * @return True to continue response processing.
     */
    auto After(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        CheckEq(response.Body(), request.ClientIp(), "aspect sees completed async content");
        return true;
    }
};

/**
 * @brief Exercise default, nonmatching, and trusted configurations over persistent loopback connections.
 */
auto client_ip_loopback() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start client IP runtime");
    using mhttp::server::TrustedProxies;
    for (auto const& policy : { TrustedProxies{}, TrustedProxies::Create({ "192.0.2.0/24" }).value(),
             TrustedProxies::Create({ "127.0.0.0/8", "10.0.0.0/8", "fd00::/8" }).value() }) {
        bool const trusted = policy.Contains("127.0.0.1");
        std::atomic<unsigned> pre{}, handlers{}, posts{};
        std::mutex records_mutex;
        std::vector<std::pair<std::string, mhttp::Endpoint>> records;
        mhttp::server::ServerOptions options{ .trusted_proxies = policy };
        mhttp::server::HttpServer server{ runtime, options };
        options.trusted_proxies = TrustedProxies{};
        Check(server.SetPreRoutingHandler([&](auto& request, auto&) -> bool {
            ++pre;
            request.Data().Store("pre_ip", request.ClientIp());
            return false;
        }).has_value(), "configure metadata observation before routing");
        Check(server.Use(ClientIpAspect{}).has_value(), "observe metadata in middleware");
        Check(server.set_http_handler<mhttp::method::Get>("/ip", [&](auto& request, auto& response) -> mhttp::Task<void> {
            auto const address = request.ClientIp();
            auto const peer = request.RemoteEndpoint();
            co_await mhttp::Sleep(1ms);
            CheckEq(request.ClientIp(), address, "address survives handler suspension");
            CheckEq(request.RemoteEndpoint(), peer, "real endpoint survives handler suspension");
            CheckEq(request.Data().template Fetch<std::string>("aspect_ip"), std::optional{ address },
                "aspect observes the same address as the handler");
            ++handlers;
            Check(response.SetContent(address, "text/plain").has_value(), "return resolved address");
        }).has_value(), "configure async IP endpoint");
        Check(server.SetPostRoutingHandler([&](auto& request, auto& response) {
            ++posts;
            Check(response.Headers().Set("X-Client-IP", request.ClientIp()).has_value(), "post-routing address");
            Check(response.Headers().Set("X-Peer", request.RemoteEndpoint().address).has_value(), "real peer address");
            Check(response.Headers().Set("X-Peer-Port", std::to_string(request.RemoteEndpoint().port)).has_value(),
                "real peer port");
        }).has_value(), "configure metadata finalizer");
        Check(server.SetAccessLogger([&](auto const& request, auto const& response, auto) {
            CheckEq(response.Body(), request.ClientIp(), "access logger sees same completed request");
            std::lock_guard lock{ records_mutex };
            records.emplace_back(request.ClientIp(), request.RemoteEndpoint());
        }).has_value(), "retain independent access metadata copies");
        auto port = server.BindToAnyPort("127.0.0.1");
        if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start IP listener" }; }
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port,
            { .timeout_policy = mhttp::client::TimeoutPolicy::Overall, .timeout = 2s } };
        /**
         * @brief Own one repeated-field chain and its expected result for a trusted peer.
         */
        struct Case {
            std::vector<std::string_view> fields; ///< Separate wire fields in insertion order.
            std::string_view selected; ///< Expected resolved IP when the peer is trusted.
        };
        std::vector<Case> const cases{
            { {}, "127.0.0.1" }, { { "203.0.113.7" }, "203.0.113.7" },
            { { "198.51.100.9, 10.0.0.5" }, "198.51.100.9" },
            { { "203.0.113.1, 203.0.113.9" }, "203.0.113.9" },
            { { "not-an-ip" }, "127.0.0.1" }, { { "127.0.0.1, 10.0.0.1" }, "127.0.0.1" },
            { { "203.0.113.1, 198.51.100.9", "10.0.0.5", "127.0.0.1" }, "198.51.100.9" },
            { { "203.0.113.1", "198.51.100.9" }, "198.51.100.9" },
            { { "bad, 198.51.100.9 \t,\t 10.0.0.5" }, "198.51.100.9" },
            { { "2001:0DB8:0:0:0:0:0:7, fd12::1" }, "2001:db8::7" },
        };
        std::optional<mhttp::Endpoint> connection;
        for (auto const& test : cases) {
            mhttp::client::Request request{ mhttp::Method::Get, "/ip" };
            for (auto value : test.fields) { Check(request.Headers().Add("x-forwarded-for", value).has_value(), "wire forwarding field"); }
            auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
            if (!Check(reply && reply->Status() == mhttp::Status::Ok, "loopback IP response")) { continue; }
            auto expected = trusted ? test.selected : "127.0.0.1";
            CheckEq(reply->Body(), expected, "public listener respects trust and chain order");
            CheckEq(reply->Header("X-Client-IP"), expected, "post-routing agrees with response");
            CheckEq(reply->Header("X-Peer"), "127.0.0.1", "real peer never becomes forwarded address");
            CheckEq(reply->Header("X-Peer-Port"), std::to_string(reply->LocalEndpoint().port), "real peer port survives forwarding");
            if (connection) { CheckEq(reply->LocalEndpoint(), *connection, "each request resolves afresh on one persistent socket"); }
            connection = reply->LocalEndpoint();
        }
        Check(server.Stop().has_value(), "stop listener and drain its access observers");
        CheckEq(pre.load(), cases.size(), "pre-routing sees every request");
        CheckEq(handlers.load(), cases.size(), "handlers see every request");
        CheckEq(posts.load(), cases.size(), "post-routing sees every request");
        CheckEq(records.size(), cases.size(), "all access callbacks completed before stop returns");
        for (std::size_t index{}; index < std::min(records.size(), cases.size()); ++index) {
            CheckEq(records[index].first, trusted ? cases[index].selected : "127.0.0.1", "retained owned client IP after drain");
            Check(connection && records[index].second == *connection, "retained real endpoint after drain");
        }
    }
    runtime.Stop();
}

/**
 * @brief Isolate per-request resolution and immutable configuration across concurrent session strands.
 */
auto client_ip_parallel() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start concurrent IP runtime");
    std::atomic<unsigned> handlers{};
    mhttp::server::HttpServer server{ runtime,
        { .trusted_proxies = mhttp::server::TrustedProxies::Create({ "127.0.0.1", "10.0.0.0/8" }).value() } };
    Check(server.set_http_handler<mhttp::method::Get>("/ip", [&](auto& request, auto& response) -> mhttp::Task<void> {
        auto const address = request.ClientIp();
        co_await mhttp::Sleep(2ms);
        CheckEq(request.ClientIp(), address, "concurrent requests retain independent IP storage");
        ++handlers;
        Check(response.SetContent(address, "text/plain").has_value(), "concurrent IP content");
    }).has_value(), "configure concurrent IP endpoint");
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start concurrent IP listener" }; }
    {
        std::vector<std::jthread> callers;
        for (unsigned index{}; index != 8; ++index) {
            callers.emplace_back([&, index] {
                mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port,
                    { .timeout_policy = mhttp::client::TimeoutPolicy::Overall, .timeout = 2s } };
                for (unsigned iteration{}; iteration != 4; ++iteration) {
                    auto const expected = std::format("198.51.100.{}", index * 4 + iteration + 1);
                    mhttp::client::Request request{ mhttp::Method::Get, "/ip" };
                    Check(request.Headers().Add("X-Forwarded-For", "203.0.113.1, " + expected + ", 10.0.0.5").has_value(),
                        "concurrent independent forwarding chain");
                    auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
                    Check(reply && reply->Body() == expected, "concurrent chain cannot leak into another request");
                }
            });
        }
    }
    CheckEq(handlers.load(), 32u, "all concurrent IP requests completed");
    Check(server.Stop().has_value(), "drain concurrent IP listener");
    runtime.Stop();
}

/**
 * @brief Verify an actual IPv6 peer requires an IPv6 policy; skip only when IPv6 cannot bind.
 */
auto client_ip_ipv6() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start IPv6 IP runtime");
    for (bool trusted : { false, true }) {
        auto policy = mhttp::server::TrustedProxies::Create(
            trusted ? std::vector<std::string>{ "::1", "fd00::/8" } : std::vector<std::string>{ "0.0.0.0/0" }).value();
        mhttp::server::HttpServer server{ runtime, { .ipv6_only = true, .trusted_proxies = policy } };
        Check(server.set_http_handler<mhttp::method::Get>("/ip", [](auto& request, auto& response) {
            CheckEq(request.RemoteEndpoint().address, "::1", "real IPv6 peer remains unchanged");
            Check(response.SetContent(request.ClientIp(), "text/plain").has_value(), "IPv6 resolved address");
        }).has_value(), "configure IPv6 IP endpoint");
        auto port = server.BindToAnyPort("::1");
        if (!port) { std::cout << "[skip] IPv6 loopback binding unavailable\n"; break; }
        if (!server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start IPv6 IP listener" }; }
        mhttp::client::HttpClient client{ runtime, "::1", *port,
            { .timeout_policy = mhttp::client::TimeoutPolicy::Overall, .timeout = 2s } };
        mhttp::client::Request request{ mhttp::Method::Get, "/ip" };
        Check(request.Headers().Add("X-Forwarded-For", "2001:db8::7, fd12::1").has_value(), "IPv6 forwarding chain");
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(reply && reply->Body() == (trusted ? "2001:db8::7" : "::1"), "IPv6 peer trust controls forwarded IP");
        Check(server.Stop().has_value(), "drain IPv6 IP listener");
    }
    runtime.Stop();
}

/**
 * @brief Run bounded public trusted proxy integration tests.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client IP loopback policy and hooks", client_ip_loopback },
        { "client IP concurrent session ownership", client_ip_parallel },
        { "client IP IPv6 loopback policy", client_ip_ipv6 },
    });
}
