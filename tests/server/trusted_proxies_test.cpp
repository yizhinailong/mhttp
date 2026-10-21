/**
 * @file trusted_proxies_test.cpp
 * @brief Verify immutable numeric proxy allowlists, strict validation, and every CIDR prefix bit.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using mhttp::server::TrustedProxies;

/**
 * @brief Check empty/single-address policies and immutable ownership across copies and replacement.
 */
auto proxy_addresses() -> void {
    TrustedProxies empty;
    Check(!empty.Contains("127.0.0.1") && !empty.Contains("::1"), "no implicit trust");
    Check(TrustedProxies::Create({}).has_value(), "empty configured policy is valid");
    std::vector<std::string> sources{ "192.0.2.7", "::1", "fd00::1" };
    auto policy = TrustedProxies::Create(sources).value();
    sources.assign({ "0.0.0.0/0" });
    Check(policy.Contains("192.0.2.7") && !policy.Contains("192.0.2.8"), "owned IPv4 singleton");
    Check(policy.Contains("0:0:0:0:0:0:0:1") && !policy.Contains("::2"), "IPv6 numeric equality");
    Check(policy.Contains("fd00::1") && !policy.Contains("fd00::2"), "owned IPv6 singleton");
    auto snapshot = policy;
    policy = empty;
    Check(snapshot.Contains("192.0.2.7") && !policy.Contains("192.0.2.7"), "replacement preserves copies");
    auto moved = std::move(snapshot);
    Check(moved.Contains("::1") && !snapshot.Contains("::1"), "move retains ownership and empties source");
}

/**
 * @brief Exercise zero/full prefixes, host bits, partial bytes, and strict mapped-address families.
 */
auto proxy_networks() -> void {
    auto policy = TrustedProxies::Create({ "127.5.6.7/8", "192.0.2.255/25", "fd12::abcd/8" }).value();
    Check(policy.Contains("127.0.0.1") && policy.Contains("127.255.255.255"), "IPv4 CIDR ignores host bits");
    Check(!policy.Contains("126.255.255.255") && !policy.Contains("128.0.0.0"), "IPv4 prefix boundaries");
    Check(policy.Contains("192.0.2.128") && !policy.Contains("192.0.2.127"), "partial-byte prefix");
    Check(policy.Contains("fdff:ffff::1") && !policy.Contains("fcff:ffff::1"), "IPv6 CIDR boundaries");
    Check(!policy.Contains("::ffff:127.0.0.1"), "IPv4 CIDR does not trust mapped IPv6");
    auto v4 = TrustedProxies::Create({ "192.0.2.1/0" }).value();
    auto v6 = TrustedProxies::Create({ "2001:db8::1/0" }).value();
    Check(v4.Contains("0.0.0.0") && v4.Contains("255.255.255.255") && !v4.Contains("::1"), "IPv4 /0 family");
    Check(v6.Contains("::") && v6.Contains("ffff::") && v6.Contains("::ffff:192.0.2.1")
        && !v6.Contains("192.0.2.1"), "IPv6 /0 family");
    auto mapped = TrustedProxies::Create({ "::ffff:192.0.2.7/120" }).value();
    Check(mapped.Contains("::ffff:192.0.2.254") && !mapped.Contains("::ffff:192.0.3.1")
        && !mapped.Contains("192.0.2.7"), "mapped addresses require an explicit IPv6 policy");
    for (unsigned bit{}; bit != 32; ++bit) {
        auto const value = std::uint32_t{ 1 } << (31 - bit);
        auto const ip = std::format("{}.{}.{}.{}", value >> 24, (value >> 16) & 255,
            (value >> 8) & 255, value & 255);
        auto wider = TrustedProxies::Create({ std::format("0.0.0.0/{}", bit) }).value();
        auto narrower = TrustedProxies::Create({ std::format("0.0.0.0/{}", bit + 1) }).value();
        Check(wider.Contains(ip) && !narrower.Contains(ip), std::format("IPv4 prefix bit {}", bit));
    }
    for (unsigned bit{}; bit != 128; ++bit) {
        std::string ip;
        for (unsigned group{}; group != 8; ++group) {
            if (group != 0) { ip += ':'; }
            ip += std::format("{:x}", group == bit / 16 ? (1u << (15 - bit % 16)) : 0u);
        }
        auto wider = TrustedProxies::Create({ std::format("::/{}", bit) }).value();
        auto narrower = TrustedProxies::Create({ std::format("::/{}", bit + 1) }).value();
        Check(wider.Contains(ip) && !narrower.Contains(ip), std::format("IPv6 prefix bit {}", bit));
    }
}

/**
 * @brief Reject ambiguous or malformed addresses/CIDRs before a policy can be published.
 */
auto proxy_invalid_sources() -> void {
    for (auto value : { "", "not-an-ip", "localhost", "127.1", "0x7f000001", "127.00.0.1", "256.0.0.1",
             " 127.0.0.1", "127.0.0.1 ", "127.0.0.1:80", "[::1]", "::1%1", "fe80::1%lo", ":::",
             "1:2:3:4:5:6:7:8:9", "::ffff:192.0.2.999", "10.0.0.0/33", "2001:db8::/129",
             "::/-1", "::/+1", "::/", "::/ 1", "::/1/2", "::/4294967296", "::/128x" }) {
        auto rejected = TrustedProxies::Create({ "127.0.0.1", value });
        Check(!rejected && rejected.error() == mhttp::Error::InvalidRequest,
            std::format("invalid source {:?} rejects entire policy", value));
        Check(!TrustedProxies::Create({ "0.0.0.0/0", "::/0" })->Contains(value),
            std::format("invalid address {:?} never matches", value));
    }
}

/**
 * @brief Build validated repeated forwarding fields in their original wire order.
 * @param values Borrowed complete field values, copied by Headers.
 * @return Owned fields for a standalone resolution scenario.
 */
auto forwarded_headers(std::initializer_list<std::string_view> values) -> mhttp::Headers {
    mhttp::Headers headers;
    for (auto value : values) { Check(headers.Add("X-Forwarded-For", value).has_value(), "forwarding field"); }
    return headers;
}

/**
 * @brief Ignore forwarding claims from untrusted peers, including other common forwarding headers.
 */
auto proxy_peer_boundary() -> void {
    auto headers = forwarded_headers({ "203.0.113.7" });
    for (auto const& policy : { TrustedProxies{}, TrustedProxies::Create({ "192.0.2.7" }).value(),
             TrustedProxies::Create({ "192.0.2.0/24" }).value() }) {
        CheckEq(policy.Resolve("127.0.0.1", headers), "127.0.0.1", "untrusted peer ignores forged address");
    }
    auto policy = TrustedProxies::Create({ "127.0.0.0/8" }).value();
    CheckEq(policy.Resolve("127.0.0.1", headers), "203.0.113.7", "CIDR trusts direct peer");
    for (auto peer : { "", "localhost", "127.1", "[::1]", "::1%1" }) {
        CheckEq(policy.Resolve(peer, headers), peer, "invalid transport peer cannot authorize headers");
    }
    mhttp::Headers other;
    Check(other.Add("Forwarded", "for=203.0.113.7").has_value(), "standard forwarding field");
    Check(other.Add("X-Real-IP", "203.0.113.7").has_value(), "other forwarding field");
    CheckEq(policy.Resolve("127.0.0.1", other), "127.0.0.1", "only X-Forwarded-For is configured");
}

/**
 * @brief Choose the nearest untrusted valid hop, retaining order across repeated fields and OWS.
 */
auto proxy_forwarded_chain() -> void {
    auto policy = TrustedProxies::Create({ "127.0.0.1", "10.0.0.0/8" }).value();
    CheckEq(policy.Resolve("127.0.0.1", forwarded_headers({ "198.51.100.9, 10.0.0.5" })),
        "198.51.100.9", "skip nearest trusted proxy");
    CheckEq(policy.Resolve("127.0.0.1", forwarded_headers({ "203.0.113.1, 203.0.113.9" })),
        "203.0.113.9", "leftmost spoof cannot replace closest untrusted hop");
    auto repeated = forwarded_headers({ "203.0.113.1, 198.51.100.9", "10.0.0.5" });
    Check(repeated.Add("x-FORWARDED-for", "127.0.0.1").has_value(), "mixed-case repeated field");
    CheckEq(policy.Resolve("127.0.0.1", repeated), "198.51.100.9", "repeated fields form one list");
    CheckEq(policy.Resolve("127.0.0.1", forwarded_headers({ "198.51.100.9 \t, \t10.0.0.5" })),
        "198.51.100.9", "trim only spaces and tabs around each token");
    CheckEq(policy.Resolve("127.0.0.1", forwarded_headers({ "203.0.113.1", "198.51.100.9" })),
        "198.51.100.9", "rightmost field is nearest to transport");
}

/**
 * @brief Preserve reference fallback/invalid-item semantics without accepting ambiguous IP spellings.
 */
auto proxy_forwarded_fallbacks() -> void {
    auto policy = TrustedProxies::Create({ "127.0.0.1", "10.0.0.0/8" }).value();
    for (auto const& headers : { forwarded_headers({}), forwarded_headers({ "not-an-ip" }),
             forwarded_headers({ "127.0.0.1" }), forwarded_headers({ "10.0.0.1, 127.0.0.1" }),
             forwarded_headers({ "", ",,," }),
             forwarded_headers({ "unknown, 127.1, 127.00.0.1, 0x7f000001, 198.51.100.9:80, [::1], ::1%1" }) }) {
        CheckEq(policy.Resolve("127.0.0.1", headers), "127.0.0.1", "no valid untrusted hop falls back");
    }
    CheckEq(policy.Resolve("127.0.0.1", forwarded_headers({ "bad, 198.51.100.9, unknown, , 10.0.0.1" })),
        "198.51.100.9", "malformed items are skipped without name resolution");
    auto all = TrustedProxies::Create({ "0.0.0.0/0", "::/0" }).value();
    CheckEq(all.Resolve("127.0.0.1", forwarded_headers({ "198.51.100.9, 2001:db8::1" })),
        "127.0.0.1", "all-trusted chain never selects leftmost by default");
}

/**
 * @brief Resolve IPv6 and mixed chains numerically, without promoting mapped addresses into IPv4.
 */
auto proxy_forwarded_ipv6() -> void {
    auto policy = TrustedProxies::Create({ "::1/128", "fd00::/8", "10.0.0.0/8" }).value();
    CheckEq(policy.Resolve("::1", forwarded_headers({ "2001:0DB8:0:0:0:0:0:7, fd12::1" })),
        "2001:db8::7", "canonical IPv6 client after trusted IPv6 hop");
    CheckEq(policy.Resolve("::1", forwarded_headers({ "198.51.100.9, 10.0.0.5, fd12::1" })),
        "198.51.100.9", "mixed address families in one chain");
    auto v4 = TrustedProxies::Create({ "0.0.0.0/0" }).value();
    CheckEq(v4.Resolve("::ffff:127.0.0.1", forwarded_headers({ "203.0.113.7" })),
        "::ffff:127.0.0.1", "IPv4 networks cannot authorize a mapped direct peer");
}

/**
 * @brief Keep a resolved client IP owned across source changes, policy replacement, and request moves.
 */
auto proxy_request_ownership() -> void {
    auto policy = TrustedProxies::Create({ "127.0.0.1" }).value();
    mhttp::detail::RequestHead head{ .target = "/ip" };
    head.headers = forwarded_headers({ "203.0.113.7" });
    auto request = mhttp::detail::ServerRequestAccess::Create(
        head, { "127.0.0.1", 1234 }, { "127.0.0.1", 8080 }, policy);
    Check(head.headers.Set("X-Forwarded-For", "198.51.100.1").has_value(), "replace caller field storage");
    policy = TrustedProxies{};
    auto moved = std::move(request);
    CheckEq(moved.ClientIp(), "203.0.113.7", "resolved address belongs to moved request");
    CheckEq(moved.Header("X-Forwarded-For"), std::string_view{ "203.0.113.7" }, "request owns original field");
    CheckEq(moved.RemoteEndpoint(), mhttp::Endpoint{ "127.0.0.1", 1234 }, "real peer is preserved");
    CheckEq(moved.LocalEndpoint(), mhttp::Endpoint{ "127.0.0.1", 8080 }, "local endpoint is preserved");
    auto standalone = mhttp::detail::ServerRequestAccess::Create({});
    Check(standalone.ClientIp().empty(), "absent standalone peer yields empty client IP");
}

/**
 * @brief Run bounded trusted proxy policy tests without network resources.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "proxy addresses and snapshots", proxy_addresses },
        { "proxy networks and prefix bits", proxy_networks },
        { "proxy invalid configuration", proxy_invalid_sources },
        { "proxy direct peer boundary", proxy_peer_boundary },
        { "proxy forwarded chain ordering", proxy_forwarded_chain },
        { "proxy forwarded fallbacks", proxy_forwarded_fallbacks },
        { "proxy forwarded IPv6", proxy_forwarded_ipv6 },
        { "proxy request ownership", proxy_request_ownership },
    });
}
