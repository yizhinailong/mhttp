/**
 * @file auth_test.cpp
 * @brief Verify issue-time default and authorization snapshots over real loopback HTTP requests.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Exercise Basic/Bearer encoding, replacement, explicit overrides, and cross-thread updates.
 */
auto authentication_snapshots() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start authentication runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Get("/", [](auto& request, auto& response) {
        for (auto name : { "Authorization", "X-Default", "X-Repeated" }) {
            Check(response.Headers().Set(name, request.Header(name)).has_value(), "echo request field");
        }
        CheckEq(request.Headers().Count("Authorization"), std::size_t{ 1 }, "one authorization value on wire");
        CheckEq(request.Headers().Count("X-Repeated"), std::size_t{ 2 }, "duplicate defaults remain ordered");
    }).has_value(), "register authentication echo");
    auto port = server.BindToAnyPort("127.0.0.1");
    Check(port && server.Start() && server.WaitUntilReady(), "start authentication peer");
    mhttp::client::ClientOptions options;
    Check(options.default_headers.Set("X-Default", "initial").has_value(), "initial default");
    Check(options.default_headers.Add("X-Repeated", "one").has_value(), "first repeated default");
    Check(options.default_headers.Add("x-repeated", "two").has_value(), "second repeated default");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port, std::move(options) };
    Check(client.SetBasicAuth("Aladdin", "open sesame").has_value(), "set Basic authorization");
    auto first = client.Get();
    std::jthread update{ [&] { Check(client.SetBearerToken("token-._~+/123==").has_value(), "set Bearer from foreign thread"); } };
    update.join();
    auto second = client.Get();
    auto basic = mhttp::SyncWait(runtime, std::move(first));
    auto bearer = mhttp::SyncWait(runtime, std::move(second));
    Check(basic && bearer, "both issued requests complete");
    if (!basic || !bearer) { return; }
    CheckEq(basic->Header("Authorization"), "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="sv, "Basic snapshot survives later mutation before awaiting");
    CheckEq(bearer->Header("Authorization"), "Bearer token-._~+/123=="sv, "later Task owns Bearer fallback");
    CheckEq(bearer->Header("X-Default"), "initial"sv, "auth mutation preserves other defaults");
    CheckEq(basic->LocalEndpoint(), bearer->LocalEndpoint(), "configuration mutation preserves connection reuse");

    for (auto token : { "", "=onlypadding", "space token", "bad\r\nInjected: yes", "a=b", "bad:token" }) {
        auto invalid = client.SetBearerToken(token);
        Check(!invalid && invalid.error() == mhttp::Error::InvalidHeaders, "reject malformed Bearer without replacement");
    }
    Check(!client.SetBasicAuth("bad:user", "pass"), "reject colon in Basic user identifier");
    mhttp::Headers invalid_host;
    Check(invalid_host.Add("Host", "bad host").has_value(), "lexically valid but semantically invalid authority");
    Check(!client.SetDefaultHeaders(std::move(invalid_host)), "reject bad default authority atomically");
    auto unchanged = mhttp::SyncWait(runtime, client.Get());
    Check(unchanged && unchanged->Header("Authorization") == "Bearer token-._~+/123==", "rejected configuration preserves all prior fields");

    mhttp::Headers explicit_fields;
    Check(explicit_fields.Set("Authorization", "Custom explicit").has_value(), "explicit credential");
    Check(explicit_fields.Set("X-Default", "request").has_value(), "explicit default override");
    auto explicit_response = mhttp::SyncWait(runtime, client.Get("/", {}, std::move(explicit_fields)));
    Check(explicit_response && explicit_response->Header("Authorization") == "Custom explicit"
        && explicit_response->Header("X-Default") == "request", "explicit values override auth and other defaults");
    Check(client.SetBasicAuth("user", "pass:word").has_value(), "Basic password permits colon");
    auto colon = mhttp::SyncWait(runtime, client.Get());
    Check(colon && colon->Header("Authorization") == "Basic dXNlcjpwYXNzOndvcmQ=", "Basic retains full colon-containing password");
}

/**
 * @brief Replace all fallback fields and prove earlier lazy Tasks retain owned values after replacement.
 */
auto default_header_replacement() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start header replacement runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Get("/", [](auto& request, auto& response) {
        Check(response.Headers().Set("X-Default", request.Header("X-Default")).has_value(), "echo configured default");
        Check(response.Headers().Set("X-Auth", request.Header("Authorization")).has_value(), "echo authorization presence");
    }).has_value(), "register replacement peer");
    auto port = server.BindToAnyPort("127.0.0.1");
    Check(port && server.Start() && server.WaitUntilReady(), "start replacement peer");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port };
    mhttp::Headers defaults;
    Check(defaults.Set("X-Default", "owned").has_value(), "create owned default");
    Check(client.SetDefaultHeaders(defaults).has_value(), "copy default configuration");
    Check(client.SetBearerToken("secret").has_value(), "set credential before issue");
    auto issued = client.Get();
    Check(defaults.Set("X-Default", "modified source").has_value(), "mutate caller's independent collection");
    Check(client.SetDefaultHeaders({}).has_value(), "clear configuration and credential");
    auto before = mhttp::SyncWait(runtime, std::move(issued));
    auto after = mhttp::SyncWait(runtime, client.Get());
    Check(before && before->Header("X-Default") == "owned" && before->Header("X-Auth") == "Bearer secret", "issued Task owns all configured fields");
    Check(after && after->Header("X-Default").empty() && after->Header("X-Auth").empty(), "replacement clears prior defaults for future Tasks");
}

/**
 * @brief Run public credential/default-field acceptance tests.
 * @param argc Argument count.
 * @param argv Optional test filters.
 * @return Bounded test process status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client authentication snapshots", authentication_snapshots },
        { "client default header replacement", default_header_replacement },
    });
}
