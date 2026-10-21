/**
 * @file middleware_builtin_test.cpp
 * @brief Verify built-in authentication factories, route short circuits, and global HTTP middleware.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Bind and start a configured middleware peer on an ephemeral loopback port.
 * @param server Longer-lived server owning all aspects and routes.
 * @return Chosen port.
 */
auto start_middleware_peer(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "middleware peer failed" }; }
    return *port;
}

/**
 * @brief Return recoverable configuration errors and escape protection-space labels without retaining input views.
 */
auto authentication_configuration() -> void {
    using mhttp::server::BasicAuth;
    using mhttp::server::BearerAuth;
    CheckEq(BasicAuth::Create(BasicAuth::Validator{}).error(), mhttp::Error::InvalidRequest, "empty Basic callback is invalid");
    CheckEq(BearerAuth::Create(BearerAuth::Validator{}).error(), mhttp::Error::InvalidRequest, "empty Bearer callback is invalid");
    CheckEq(BasicAuth::Create("bad:user", "password").error(), mhttp::Error::InvalidRequest, "Basic user identifier excludes colon");
    for (auto token : { "", "=padding", "has space", "bad:token", "a=b", "a\r\nb" }) {
        CheckEq(BearerAuth::Create(token).error(), mhttp::Error::InvalidHeaders, "fixed Bearer token must satisfy b64token grammar");
    }
    for (auto realm : { "realm\r\nInjected: yes"sv, "bad\0realm"sv, "bad\x7f"sv }) {
        CheckEq(BasicAuth::Create("user", "password", realm).error(), mhttp::Error::InvalidHeaders, "Basic challenge rejects unsafe realm");
        CheckEq(BearerAuth::Create("token", realm).error(), mhttp::Error::InvalidHeaders, "Bearer challenge rejects unsafe realm");
    }
    auto basic = BasicAuth::Create("user", "password", "area \"quote\" \\ path");
    Check(basic.has_value(), "quoted realm is accepted through escaping");
    auto request = mhttp::detail::ServerRequestAccess::Create({});
    mhttp::server::Response response;
    Check(!basic->Before(request, response), "missing Basic credential is denied");
    CheckEq(response.Status(), mhttp::Status::Unauthorized, "factory challenge denial is 401");
    CheckEq(response.Header("WWW-Authenticate"), "Basic realm=\"area \\\"quote\\\" \\\\ path\", charset=\"UTF-8\""sv, "realm quotes and backslash are escaped");
}

/**
 * @brief Verify fixed Basic and Bearer credentials over real HTTP, including malformed schemes and duplicate fields.
 */
auto route_authentication() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start route authentication runtime");
    mhttp::server::HttpServer server{ runtime };
    std::atomic<unsigned> calls{};
    auto handler = [&calls](auto&, auto& response) {
        ++calls;
        Check(response.SetContent("protected", "text/plain").has_value(), "authorized handler response");
    };
    std::string user{ "Aladdin" }, password{ "open:sesame" }, token{ "secret._~+/123==" };
    auto basic = mhttp::server::BasicAuth::Create(user, password, "basic-area");
    auto bearer = mhttp::server::BearerAuth::Create(token, "bearer-area");
    Check(basic && bearer, "create route authentication aspects");
    Check(server.Get("/basic", handler, *basic).has_value(), "register Basic route aspect");
    Check(server.Get("/bearer", handler, *bearer).has_value(), "register Bearer route aspect");
    user = password = token = "caller changed its source";
    auto port = start_middleware_peer(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    auto const valid_basic = "Basic " + mhttp::Base64Encode("Aladdin:open:sesame");
    for (auto const& [path, value, accepted] : std::to_array<std::tuple<std::string_view, std::string, bool>>({
             { "/basic", "", false }, { "/basic", "Basic !!!!", false },
             { "/basic", "Basic " + mhttp::Base64Encode("nocolon"), false },
             { "/basic", "Basic " + mhttp::Base64Encode("Aladdin:wrong"), false },
             { "/basic", "Bearer token", false }, { "/basic", valid_basic, true },
             { "/basic", "bAsIc\t" + mhttp::Base64Encode("Aladdin:open:sesame"), true },
             { "/bearer", "", false }, { "/bearer", "Basic secret._~+/123==", false },
             { "/bearer", "BearerX secret._~+/123==", false }, { "/bearer", "Bearersecret._~+/123==", false },
             { "/bearer", "Bearer wrong", false }, { "/bearer", "Bearer secret token", false },
             { "/bearer", "Bearer secret._~+/123==", true }, { "/bearer", "bEaReR  secret._~+/123==", true } })) {
        mhttp::Headers headers;
        if (!value.empty()) { Check(headers.Set("Authorization", value).has_value(), "explicit authorization test value"); }
        auto const before = calls.load();
        auto response = mhttp::SyncWait(runtime, client.Get(std::string{ path }, {}, std::move(headers)));
        Check(response.has_value(), "authentication returns an ordinary HTTP response");
        if (!response) { continue; }
        CheckEq(response->Status(), accepted ? mhttp::Status::Ok : mhttp::Status::Unauthorized, "correct and rejected credentials receive expected status");
        CheckEq(calls.load(), before + static_cast<unsigned>(accepted), "unauthorized requests never invoke protected handler");
        if (accepted) { Check(response->Body() == "protected" && !response->Headers().Has("WWW-Authenticate"), "authorized response is unmodified"); }
        else { Check(response->Header("WWW-Authenticate").starts_with(path == "/basic" ? "Basic realm=\"basic-area\"" : "Bearer realm=\"bearer-area\""), "401 carries correct scheme and realm"); }
    }
    for (auto path : { "/basic", "/bearer" }) {
        mhttp::Headers duplicates;
        auto const value = path == "/basic"sv ? valid_basic : "Bearer secret._~+/123==";
        Check(duplicates.Add("Authorization", value).has_value(), "first credential");
        Check(duplicates.Add("authorization", value).has_value(), "duplicate credential");
        auto result = mhttp::SyncWait(runtime, client.Get(path, {}, std::move(duplicates)));
        Check(result && result->Status() == mhttp::Status::Unauthorized, "ambiguous repeated Authorization is denied");
    }
    Check(client.SetBasicAuth("Aladdin", "open:sesame").has_value(), "configure client-side Basic helper");
    auto basic_response = mhttp::SyncWait(runtime, client.Get("/basic"));
    Check(basic_response && basic_response->Status() == mhttp::Status::Ok, "client Basic helper interoperates with server aspect");
    Check(client.SetBearerToken("secret._~+/123==").has_value(), "configure client Bearer helper");
    auto bearer_response = mhttp::SyncWait(runtime, client.Get("/bearer"));
    Check(bearer_response && bearer_response->Status() == mhttp::Status::Ok, "client Bearer helper interoperates with server aspect");
}

/**
 * @brief Apply custom validators globally, avoid invoking them for invalid syntax, and retain normal exception handling.
 */
auto global_authentication() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start global authentication runtime");
    mhttp::server::HttpServer server{ runtime };
    std::atomic<unsigned> validated{}, handled{};
    auto auth = mhttp::server::BearerAuth::Create([&validated](std::string_view token) {
        ++validated;
        if (token == "throw") { throw std::runtime_error{ "validator failure" }; }
        return token == "allowed";
    });
    Check(auth && server.Use(*auth), "install global custom Bearer validator");
    for (auto path : { "/a", "/b" }) {
        Check(server.Get(path, [&handled](auto&, auto& response) {
            ++handled;
            Check(response.SetContent("global protected", "text/plain").has_value(), "global protected response");
        }).has_value(), "register globally protected route");
    }
    auto port = start_middleware_peer(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    for (auto path : { "/a", "/b" }) {
        auto denied = mhttp::SyncWait(runtime, client.Get(path));
        Check(denied && denied->Status() == mhttp::Status::Unauthorized, "global middleware applies to every matched route");
    }
    CheckEq(validated.load(), 0U, "missing/malformed credentials do not invoke application validator");
    Check(client.SetBearerToken("allowed").has_value(), "configure allowed token");
    for (auto path : { "/a", "/b" }) {
        auto allowed = mhttp::SyncWait(runtime, client.Get(path));
        Check(allowed && allowed->Status() == mhttp::Status::Ok, "custom global validator accepts correct token");
    }
    CheckEq(handled.load(), 2U, "only authenticated global requests reach handlers");
    Check(client.SetBearerToken("throw").has_value(), "configure throwing validator input");
    auto failed = mhttp::SyncWait(runtime, client.Get("/a"));
    Check(failed && failed->Status() == mhttp::Status::InternalServerError, "validator exceptions reach normal router exception handling");
    auto basic = mhttp::server::BasicAuth::Create([](std::string_view username, std::string_view password) {
        return username == "custom" && password == "password:with:colons";
    });
    mhttp::detail::RequestHead head;
    Check(head.headers.Set("Authorization", "Basic " + mhttp::Base64Encode("custom:password:with:colons")).has_value(), "custom Basic credential");
    auto request = mhttp::detail::ServerRequestAccess::Create(std::move(head));
    mhttp::server::Response response;
    Check(basic && basic->Before(request, response), "custom Basic validator receives decoded user and full password");
}

/**
 * @brief Run bounded built-in middleware scenarios.
 * @param argc Argument count.
 * @param argv Optional test filters.
 * @return Test process status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "authentication configuration", authentication_configuration },
        { "route Basic and Bearer authentication", route_authentication },
        { "global authentication validators", global_authentication },
    });
}
