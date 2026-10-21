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
    });
}
