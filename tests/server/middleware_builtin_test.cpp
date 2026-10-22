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
using namespace std::chrono_literals;

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
    Check(server.set_http_handler<mhttp::method::Get>("/basic", handler, *basic).has_value(), "register Basic route aspect");
    Check(server.set_http_handler<mhttp::method::Get>("/bearer", handler, *bearer).has_value(), "register Bearer route aspect");
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
        Check(server.set_http_handler<mhttp::method::Get>(path, [&handled](auto&, auto& response) {
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
 * @brief Count one cache-variation token across repeated and comma-delimited response fields.
 * @param response Borrowed client response.
 * @param expected Case-insensitive token.
 * @return Number of occurrences.
 */
auto vary_count(mhttp::client::Response const& response, std::string_view expected) -> unsigned {
    unsigned count{};
    for (auto value : response.Headers().Values("Vary")) {
        for (auto token : mhttp::utils::split(value, ',')) {
            if (mhttp::utils::iequals(mhttp::utils::trim_ows(token), expected)) { ++count; }
        }
    }
    return count;
}

/**
 * @brief Validate origin policy and explicitly expand wildcards without authorizing credentials implicitly.
 */
auto cors_configuration() -> void {
    for (auto origin : { "", "https://app.example/", "https://user@app.example", "https://app.example?q=1",
                         "https://app.example#fragment", "https://app.example:65536", "https://one https://two", "https://app\r\nInjected: yes" }) {
        Check(!mhttp::server::Cors::Create({ .origins = { origin } }), "reject invalid serialized CORS origins");
    }
    Check(!mhttp::server::Cors::Create({ .origins = { "*", "https://app.example" } }), "wildcard must stand alone");
    Check(!mhttp::server::Cors::Create({ .methods = { "GET POST" } }), "method list contains individual tokens");
    Check(!mhttp::server::Cors::Create({ .headers = { "bad\r\nname" } }), "request names cannot inject headers");
    Check(!mhttp::server::Cors::Create({ .max_age = -1s }), "reject negative preflight cache age");
    Check(!mhttp::server::Cors::Create({ .expose_headers = { "*" }, .credentials = true }), "credentialed exposure needs explicit names");
    Check(mhttp::server::Cors::Create({ .origins = { "https://[::1]:8080", "null" } }).has_value(), "IPv6 and opaque origins are accepted explicitly");
    for (bool explicit_authorization : { false, true }) {
        mhttp::server::CorsOptions options{ .methods = { "*" }, .headers = { "*" }, .credentials = true };
        if (explicit_authorization) { options.headers.emplace_back("Authorization"); }
        auto cors = mhttp::server::Cors::Create(options);
        Check(cors.has_value(), "create wildcard credentialed policy");
        for (auto name : { "X-Custom", "Authorization" }) {
            mhttp::detail::RequestHead head{ .method = "OPTIONS" };
            Check(head.headers.Set("Origin", "https://app.example").has_value(), "set wildcard origin");
            Check(head.headers.Set("Access-Control-Request-Method", "CUSTOM").has_value(), "custom preflight method");
            Check(head.headers.Set("Access-Control-Request-Headers", name).has_value(), "requested wildcard header");
            auto request = mhttp::detail::ServerRequestAccess::Create(std::move(head));
            mhttp::server::Response response;
            Check(!cors->Before(request, response), "wildcard preflight always completes before handler");
            bool const accepted = explicit_authorization || name != "Authorization"sv;
            CheckEq(response.Status(), accepted ? mhttp::Status::NoContent : mhttp::Status::Forbidden, "Authorization requires explicit permission even with wildcard headers");
            CheckEq(response.Header("Access-Control-Allow-Origin"), "https://app.example"sv, "credentialed wildcard reflects single origin");
            if (accepted) {
                CheckEq(response.Header("Access-Control-Allow-Methods"), "CUSTOM"sv, "wildcard method expands to explicit requested method");
                CheckEq(response.Header("Access-Control-Allow-Headers"), std::string_view{ name }, "wildcard headers expand to explicit requested names");
            }
        }
    }
}

/**
 * @brief Apply exact origin allowlists, credentials, exposure, and cache variation to actual and missing routes.
 */
auto cors_actual_requests() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start actual CORS runtime");
    mhttp::server::HttpServer server{ runtime };
    mhttp::server::CorsOptions options{ .origins = { "https://one.example", "https://two.example" },
        .expose_headers = { "X-Trace" }, .credentials = true };
    auto cors = mhttp::server::Cors::Create(options);
    Check(cors && server.Use(*cors), "install exact-origin CORS globally");
    options.origins.clear();
    std::atomic<unsigned> handled{};
    Check(server.set_http_handler<mhttp::method::Get>("/data", [&handled](auto&, auto& response) {
        ++handled;
        Check(response.Headers().Set("Vary", "Accept-Encoding").has_value(), "handler supplies independent cache variation");
        Check(response.Headers().Set("X-Trace", "trace").has_value(), "exposed handler field");
        Check(response.SetContent("actual", "text/plain").has_value(), "actual CORS response");
    }).has_value(), "register actual CORS route");
    auto port = start_middleware_peer(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    for (auto origin : { "", "https://one.example", "https://two.example", "https://unknown.example", "null" }) {
        mhttp::Headers fields;
        if (*origin) { Check(fields.Set("Origin", origin).has_value(), "set actual Origin"); }
        auto result = mhttp::SyncWait(runtime, client.Get("/data", {}, std::move(fields)));
        Check(result && result->Body() == "actual", "CORS leaves ordinary routing intact for missing/denied origins");
        if (!result) { continue; }
        bool const allowed = origin == "https://one.example"sv || origin == "https://two.example"sv;
        CheckEq(result->Header("Access-Control-Allow-Origin"), allowed ? std::string_view{ origin } : ""sv, "multiple origins reflect only an allowed request origin");
        CheckEq(result->Header("Access-Control-Allow-Credentials"), allowed ? "true"sv : ""sv, "credentials granted only with allowed Origin");
        CheckEq(result->Header("Access-Control-Expose-Headers"), allowed ? "X-Trace"sv : ""sv, "expose response names only for allowed origins");
        CheckEq(vary_count(*result, "Origin"), 1U, "Origin cache variation survives handler overwrite without duplicates");
        CheckEq(vary_count(*result, "Accept-Encoding"), 1U, "independent handler Vary token survives CORS merge");
    }
    CheckEq(handled.load(), 5U, "CORS origin permission is independent of application authentication");
    mhttp::Headers fields;
    Check(fields.Set("Origin", "https://one.example").has_value(), "allowed Origin for missing route");
    auto missing = mhttp::SyncWait(runtime, client.Get("/missing", {}, fields));
    Check(missing && missing->Status() == mhttp::Status::NotFound
        && missing->Header("Access-Control-Allow-Origin") == "https://one.example", "global CORS applies to default route errors");
    Check(fields.Set("Origin", "https://unknown.example").has_value(), "denied preflight Origin");
    Check(fields.Set("Access-Control-Request-Method", "GET").has_value(), "denied-origin preflight method");
    auto denied = mhttp::SyncWait(runtime, client.Options("/data", {}, fields));
    Check(denied && denied->Status() == mhttp::Status::Forbidden && !denied->Headers().Has("Access-Control-Allow-Origin"), "preflight from denied origin is rejected without permission");
}

/**
 * @brief Answer real preflights without an OPTIONS route and distinguish malformed, denied, and ordinary OPTIONS.
 */
auto cors_preflights() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start CORS preflight runtime");
    mhttp::server::HttpServer server{ runtime };
    auto cors = mhttp::server::Cors::Create({ .methods = { "GET", "PATCH", "CUSTOM" },
        .headers = { "Content-Type", "X-Tenant", "Authorization" }, .max_age = 600s });
    Check(cors && server.Use(*cors), "install configurable wildcard CORS");
    std::atomic<unsigned> calls{};
    Check(server.set_http_handler<mhttp::method::Get>("/resource", [&calls](auto&, auto&) { ++calls; }).has_value(), "GET-only resource");
    Check(server.set_http_handler<mhttp::method::Options>("/ordinary", [&calls](auto&, auto&) { ++calls; }).has_value(), "ordinary OPTIONS route");
    auto port = start_middleware_peer(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    mhttp::Headers fields;
    Check(fields.Set("Origin", "https://app.example").has_value(), "preflight origin");
    Check(fields.Set("Access-Control-Request-Method", "PATCH").has_value(), "preflight custom method");
    Check(fields.Set("Access-Control-Request-Headers", "x-tEnAnT, Authorization").has_value(), "case-insensitive requested header names");
    auto response = mhttp::SyncWait(runtime, client.Options("/resource", {}, fields));
    Check(response && response->Status() == mhttp::Status::NoContent && response->Body().empty(), "valid preflight overrides pending 405 with empty 204");
    if (response) {
        CheckEq(response->Header("Access-Control-Allow-Origin"), "*"sv, "noncredentialed wildcard remains wildcard");
        Check(!response->Headers().Has("Access-Control-Allow-Credentials"), "credentials disabled by default");
        CheckEq(response->Header("Access-Control-Allow-Methods"), "GET, PATCH, CUSTOM"sv, "configured method list is emitted");
        CheckEq(response->Header("Access-Control-Allow-Headers"), "Content-Type, X-Tenant, Authorization"sv, "configured request header list is emitted");
        CheckEq(response->Header("Access-Control-Max-Age"), "600"sv, "preflight cache age is emitted");
        for (auto token : { "Origin", "Access-Control-Request-Method", "Access-Control-Request-Headers" }) {
            CheckEq(vary_count(*response, token), 1U, "preflight Vary contains each input exactly once");
        }
    }
    for (auto const& [name, value, expected] : std::to_array<std::tuple<std::string_view, std::string_view, mhttp::Status>>({
             { "Access-Control-Request-Method", "DELETE", mhttp::Status::Forbidden },
             { "Access-Control-Request-Method", "patch", mhttp::Status::Forbidden },
             { "Access-Control-Request-Method", "GET POST", mhttp::Status::BadRequest },
             { "Access-Control-Request-Headers", "X-Denied", mhttp::Status::Forbidden },
             { "Access-Control-Request-Headers", "X-Tenant,,Content-Type", mhttp::Status::BadRequest },
             { "Origin", "https://one https://two", mhttp::Status::BadRequest } })) {
        auto changed = fields;
        Check(changed.Set(name, value).has_value(), "configure denied/malformed preflight");
        auto result = mhttp::SyncWait(runtime, client.Options("/resource", {}, std::move(changed)));
        Check(result && result->Status() == expected, "distinguish malformed 400 from policy-denied 403");
    }
    for (auto name : { "Origin", "Access-Control-Request-Method" }) {
        auto duplicate = fields;
        Check(duplicate.Add(name, fields.Get(name)).has_value(), "duplicate singleton preflight field");
        auto result = mhttp::SyncWait(runtime, client.Options("/resource", {}, std::move(duplicate)));
        Check(result && result->Status() == mhttp::Status::BadRequest, "ambiguous preflight singleton is rejected");
    }
    CheckEq(calls.load(), 0U, "completed and rejected preflights never execute handlers");
    fields.Erase("Access-Control-Request-Method");
    auto ordinary = mhttp::SyncWait(runtime, client.Options("/ordinary", {}, fields));
    Check(ordinary && ordinary->Status() == mhttp::Status::Ok, "OPTIONS without requested method is ordinary routing");
    fields.Erase("Origin");
    Check(fields.Set("Access-Control-Request-Method", "GET").has_value(), "method without Origin");
    auto no_origin = mhttp::SyncWait(runtime, client.Options("/ordinary", {}, fields));
    Check(no_origin && no_origin->Status() == mhttp::Status::Ok && !no_origin->Headers().Has("Access-Control-Allow-Origin"), "OPTIONS without Origin stays ordinary and gets no permission");
    CheckEq(calls.load(), 2U, "only ordinary OPTIONS invokes its handler");
}

/**
 * @brief Permit credential-free browser preflights while applying authentication to actual cross-origin requests.
 */
auto cors_and_authentication() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start combined CORS/auth runtime");
    mhttp::server::HttpServer server{ runtime };
    auto cors = mhttp::server::Cors::Create({ .origins = { "https://app.example" }, .credentials = true });
    auto auth = mhttp::server::BearerAuth::Create("secret");
    Check(cors && auth && server.Use(*cors, *auth), "install CORS before authentication");
    std::atomic<unsigned> handled{};
    Check(server.set_http_handler<mhttp::method::Get>("/secret", [&handled](auto&, auto& response) {
        ++handled;
        Check(response.SetContent("authorized", "text/plain").has_value(), "combined authorized response");
    }).has_value(), "register combined protected route");
    auto port = start_middleware_peer(server);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
    mhttp::Headers fields;
    Check(fields.Set("Origin", "https://app.example").has_value(), "combined Origin");
    Check(fields.Set("Access-Control-Request-Method", "GET").has_value(), "combined preflight method");
    Check(fields.Set("Access-Control-Request-Headers", "Authorization").has_value(), "combined requested credential name");
    auto preflight = mhttp::SyncWait(runtime, client.Options("/secret", {}, fields));
    Check(preflight && preflight->Status() == mhttp::Status::NoContent && !preflight->Headers().Has("WWW-Authenticate"), "preflight completes before Bearer challenge");
    fields.Erase("Access-Control-Request-Method");
    fields.Erase("Access-Control-Request-Headers");
    auto denied = mhttp::SyncWait(runtime, client.Get("/secret", {}, fields));
    Check(denied && denied->Status() == mhttp::Status::Unauthorized
        && denied->Header("Access-Control-Allow-Origin") == "https://app.example"
        && denied->Header("Access-Control-Allow-Credentials") == "true", "actual request still requires authentication and 401 retains CORS fields");
    CheckEq(handled.load(), 0U, "neither preflight nor unauthenticated actual request executes protected handler");
    Check(client.SetBearerToken("secret").has_value(), "configure actual credential");
    auto allowed = mhttp::SyncWait(runtime, client.Get("/secret", {}, fields));
    Check(allowed && allowed->Body() == "authorized" && vary_count(*allowed, "Origin") == 1, "authenticated cross-origin response includes merged cache variation");
    CheckEq(handled.load(), 1U, "authorized actual request executes protected handler once");
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
        { "CORS configuration and wildcards", cors_configuration },
        { "CORS actual requests and cache variation", cors_actual_requests },
        { "CORS configurable preflights", cors_preflights },
        { "CORS combined with authentication", cors_and_authentication },
    });
}
