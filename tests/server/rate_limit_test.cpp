/**
 * @file rate_limit_test.cpp
 * @brief Verify shared quotas, fixed windows, bounded LRU tracking, capacity changes, and concurrent admission.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;
using mhttp::server::RateLimit;

/**
 * @brief Create an owned synthetic request with a numeric transport client address.
 * @param address Numeric IPv4/IPv6 source, copied into the request.
 * @return Owned request with resolved ClientIp.
 */
auto rate_request(std::string address) -> mhttp::server::Request {
    return mhttp::detail::ServerRequestAccess::Create({}, { std::move(address), 1234 });
}

/**
 * @brief Apply one request to shared middleware with a fresh response.
 * @param limiter Shared quota instance.
 * @param address Numeric client identity.
 * @return Whether the request is accepted.
 */
auto rate_accept(RateLimit const& limiter, std::string address) -> bool {
    auto request = rate_request(std::move(address));
    mhttp::server::Response response;
    auto const accepted = limiter.Before(request, response);
    if (!accepted) {
        CheckEq(response.Status(), mhttp::Status::TooManyRequests, "denial is HTTP 429");
        Check(!response.Header("Retry-After").empty() && response.Header("Retry-After") != "0", "denial includes positive retry hint");
    }
    return accepted;
}

/**
 * @brief Reject invalid quota, duration, capacity, and enum configuration without allocating client state.
 */
auto rate_configuration() -> void {
    for (unsigned index{}; index < 8; ++index) {
        mhttp::server::RateLimitOptions options;
        if (index == 0) { options.max_requests = 0; }
        if (index == 1) { options.max_tracked_clients = 0; }
        if (index == 2) { options.window = 0ms; }
        if (index == 3) { options.window = -1ms; }
        if (index == 4) { options.window = std::chrono::milliseconds::max(); }
        if (index == 5) { options.idle_expiration = -1ms; }
        if (index == 6) { options.idle_expiration = std::chrono::milliseconds::max(); }
        if (index == 7) { options.when_full = static_cast<mhttp::server::RateLimitCapacityAction>(100); }
        auto invalid = RateLimit::Create(options);
        Check(!invalid && invalid.error() == mhttp::Error::InvalidRequest, "invalid limiter configuration rejected");
    }
}

/**
 * @brief Admit exactly the configured quota, round fractional Retry-After upward, and leave accepted responses untouched.
 */
auto exact_quota() -> void {
    auto limiter = RateLimit::Create({ .max_requests = 3, .window = 1500ms });
    auto request = rate_request("192.0.2.1");
    mhttp::server::Response response;
    Check(response.SetContent("untouched", "text/plain").has_value(), "prepare earlier response");
    for (unsigned index{}; index < 3; ++index) {
        Check(limiter->Before(request, response), "request within quota accepted");
        CheckEq(response.Body(), std::string_view{ "untouched" }, "accepted response preserved");
    }
    Check(!limiter->Before(request, response), "first excess request rejected");
    CheckEq(response.Header("Retry-After"), std::string_view{ "2" }, "fractional seconds round up");
    Check(!limiter->Before(request, response), "rejected requests cannot wrap the quota counter");
    CheckEq(limiter->TrackedClients(), std::size_t{ 1 }, "one retained client");
}

/**
 * @brief Share counters across aspect copies while independently created middleware retains a separate quota.
 */
auto shared_quota() -> void {
    auto limiter = *RateLimit::Create({ .max_requests = 2, .window = 60s });
    auto copy = limiter;
    Check(rate_accept(limiter, "192.0.2.1") && rate_accept(copy, "192.0.2.1"), "copies share available quota");
    Check(!rate_accept(limiter, "192.0.2.1") && !rate_accept(copy, "192.0.2.1"), "copies share exhausted quota");
    auto independent = *RateLimit::Create({ .max_requests = 2, .window = 60s });
    Check(rate_accept(independent, "192.0.2.1"), "separate factories have independent counts");
    Check(rate_accept(limiter, "192.0.2.2"), "different client has independent quota");
}

/**
 * @brief Reset an expired fixed quota even when a rejected request refreshed its idle timestamp.
 */
auto fixed_window_reset() -> void {
    auto limiter = *RateLimit::Create({ .max_requests = 1, .window = 300ms, .idle_expiration = 0ms });
    Check(rate_accept(limiter, "192.0.2.1"), "start fixed window");
    std::this_thread::sleep_for(200ms);
    Check(!rate_accept(limiter, "192.0.2.1"), "denial inside original window");
    std::this_thread::sleep_for(200ms);
    Check(rate_accept(limiter, "192.0.2.1"), "denial does not extend the original quota window");
    Check(!rate_accept(limiter, "192.0.2.1"), "reset window still counts its first request");
}

/**
 * @brief Reclaim expired buckets lazily even before quota expiry, and preserve them when idle expiry is disabled.
 */
auto idle_reclamation() -> void {
    for (bool enabled : { false, true }) {
        auto limiter = *RateLimit::Create({ .max_requests = 1, .window = 60s, .idle_expiration = enabled ? 30ms : 0ms });
        Check(rate_accept(limiter, "192.0.2.1"), "start idle bucket");
        Check(!rate_accept(limiter, "192.0.2.1"), "exhaust idle bucket");
        std::this_thread::sleep_for(80ms);
        CheckEq(limiter.TrackedClients(), std::size_t{ 1 }, "observation does not trigger lazy reclamation");
        CheckEq(rate_accept(limiter, "192.0.2.1"), enabled, "idle reclamation obeys configured enablement");
    }
    auto limiter = *RateLimit::Create({ .max_requests = 1, .window = 40ms });
    Check(rate_accept(limiter, "192.0.2.1"), "default idle age starts with window");
    std::this_thread::sleep_for(90ms);
    Check(rate_accept(limiter, "192.0.2.2") && limiter.TrackedClients() == 1, "default idle expiration follows window duration");
}

/**
 * @brief Bound storage under address rotation and count every newly admitted client immediately.
 */
auto bounded_rotation() -> void {
    auto limiter = *RateLimit::Create({ .max_requests = 1, .window = 60s, .max_tracked_clients = 8, .idle_expiration = 0ms });
    for (unsigned index{}; index < 500; ++index) {
        auto address = std::format("198.51.{}.{}", index / 256, index % 256);
        Check(rate_accept(limiter, address), "new client admitted by bounded eviction");
        Check(!rate_accept(limiter, address), "new client is counted even at full capacity");
        Check(limiter.TrackedClients() <= 8, "tracking never exceeds hard capacity");
    }
    CheckEq(limiter.TrackedClients(), std::size_t{ 8 }, "full table remains bounded");
}

/**
 * @brief Evict by access order, including denied requests, so a frequently accessed bucket retains its quota.
 */
auto access_order_eviction() -> void {
    auto limiter = *RateLimit::Create({ .max_requests = 1, .window = 60s, .max_tracked_clients = 2, .idle_expiration = 0ms });
    Check(rate_accept(limiter, "192.0.2.1") && rate_accept(limiter, "192.0.2.2"), "fill LRU buckets");
    Check(!rate_accept(limiter, "192.0.2.1"), "denial refreshes first client's access order");
    Check(rate_accept(limiter, "192.0.2.3"), "new client evicts least recently accessed bucket");
    Check(!rate_accept(limiter, "192.0.2.1"), "hot first bucket retains its exhausted quota");
    Check(rate_accept(limiter, "192.0.2.2"), "evicted second bucket receives a new counted window");
}

/**
 * @brief Preserve retained quota counters across capacity updates and evict immediately when shrinking.
 */
auto capacity_changes() -> void {
    auto limiter = *RateLimit::Create({ .max_requests = 2, .window = 60s, .max_tracked_clients = 2, .idle_expiration = 0ms });
    Check(rate_accept(limiter, "192.0.2.1") && rate_accept(limiter, "192.0.2.2"), "fill resizable buckets");
    auto copy = limiter;
    Check(copy.SetMaxTrackedClients(3).has_value(), "increase shared capacity");
    Check(rate_accept(limiter, "192.0.2.1") && !rate_accept(copy, "192.0.2.1"), "capacity growth preserves existing count");
    Check(limiter.SetMaxTrackedClients(1).has_value() && copy.TrackedClients() == 1, "shrink is immediate across copies");
    Check(!rate_accept(limiter, "192.0.2.1"), "shrink preserves newest bucket's count");
    CheckEq(copy.SetMaxTrackedClients(0).error(), mhttp::Error::InvalidRequest, "zero capacity update rejected");
    Check(!rate_accept(limiter, "192.0.2.1"), "invalid update preserves existing state");
}

/**
 * @brief Reject unseen clients at capacity while retaining known counts, permitting entry after idle reclamation.
 */
auto reject_capacity() -> void {
    auto limiter = *RateLimit::Create({ .max_requests = 1, .window = 60s, .max_tracked_clients = 1,
        .idle_expiration = 40ms, .when_full = mhttp::server::RateLimitCapacityAction::Reject });
    Check(rate_accept(limiter, "192.0.2.1"), "occupy reject-policy bucket");
    for (unsigned index{ 2 }; index < 20; ++index) {
        Check(!rate_accept(limiter, std::format("192.0.2.{}", index)), "unseen client rejected by full table");
    }
    Check(!rate_accept(limiter, "192.0.2.1") && limiter.TrackedClients() == 1, "full-table denial preserves known client's quota");
    std::this_thread::sleep_for(90ms);
    Check(rate_accept(limiter, "192.0.2.2"), "idle reclamation happens before reject-policy capacity check");
    Check(!rate_accept(limiter, "192.0.2.2"), "newly admitted client is counted under reject policy");
}

/**
 * @brief Own lookup keys independently of the request and synchronize quotas across concurrent aspect copies.
 */
auto owned_keys_and_concurrency() -> void {
    auto limiter = *RateLimit::Create({ .max_requests = 37, .window = 60s });
    std::atomic<unsigned> accepted{};
    std::vector<std::jthread> callers;
    for (unsigned index{}; index < 8; ++index) {
        callers.emplace_back([copy = limiter, &accepted] {
            for (unsigned iteration{}; iteration < 100; ++iteration) {
                if (rate_accept(copy, std::string{ "2001:db8::1" })) { ++accepted; }
            }
        });
    }
    callers.clear();
    CheckEq(accepted.load(), 37u, "concurrent requests admit exactly one shared quota");
    CheckEq(limiter.TrackedClients(), std::size_t{ 1 }, "destroyed request storage cannot invalidate owned lookup keys");
}

/**
 * @brief Run bounded limiter unit cases.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "rate configuration", rate_configuration }, { "exact quota and retry rounding", exact_quota },
        { "shared quota copies", shared_quota }, { "fixed window reset", fixed_window_reset },
        { "lazy idle reclamation", idle_reclamation }, { "bounded address rotation", bounded_rotation },
        { "access order eviction", access_order_eviction }, { "capacity changes preserve counts", capacity_changes },
        { "reject unseen clients at capacity", reject_capacity }, { "owned keys and concurrent quotas", owned_keys_and_concurrency },
    });
}
