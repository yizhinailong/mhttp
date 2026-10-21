/**
 * @file router_test.cpp
 * @brief Verify public registration, response dispatch, configuration guards, and owned route lifetimes.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using mhttp::detail::RouterAccess;
using mhttp::detail::ServerRequestAccess;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

/**
 * @brief Route an owned request and return its buffered response after all callback work finishes.
 * @param runtime Running executor borrowed through completion.
 * @param router Configured route owner.
 * @param method Wire method token.
 * @param target Encoded request target.
 * @return Owned buffered response.
 */
auto dispatch(
    mhttp::Runtime& runtime,
    mhttp::server::Router& router,
    std::string_view method,
    std::string_view target
) -> mhttp::server::Response {
    auto request = ServerRequestAccess::Create({ .method = std::string{ method }, .target = std::string{ target } });
    mhttp::server::Response response;
    mhttp::SyncWait(runtime, RouterAccess::Dispatch(router, request, response));
    return response;
}

/**
 * @brief Exercise method shortcuts, generic extension methods, deterministic Allow, and default errors.
 */
auto router_methods() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start method routing runtime");
    mhttp::server::Router router;
    auto handler = [](auto& request, auto& response) {
        Check(response.SetContent(std::string{ request.MethodString() }, "text/plain").has_value(), "method response");
    };
    Check(router.Get("/verbs", handler).has_value(), "GET shortcut");
    Check(router.Head("/verbs", handler).has_value(), "HEAD shortcut");
    Check(router.Post("/verbs", handler).has_value(), "POST shortcut");
    Check(router.Put("/verbs", handler).has_value(), "PUT shortcut");
    Check(router.Patch("/verbs", handler).has_value(), "PATCH shortcut");
    Check(router.Delete("/verbs", handler).has_value(), "DELETE shortcut");
    Check(router.Options("/verbs", handler).has_value(), "OPTIONS shortcut");
    Check(router.Route(mhttp::Method::PropFind, "/verbs", handler).has_value(), "generic method registration");
    Check(router.Options("*", handler).has_value(), "OPTIONS asterisk registration");
    for (auto method : { "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "PROPFIND" }) {
        auto response = dispatch(runtime, router, method, "/verbs");
        CheckEq(response.Status(), mhttp::Status::Ok, "registered method status");
        CheckEq(response.Body(), method, "correct method callback");
    }
    CheckEq(dispatch(runtime, router, "OPTIONS", "*").Body(), "OPTIONS"sv, "literal asterisk callback");
    auto wrong_method = dispatch(runtime, router, "TRACE", "/verbs");
    CheckEq(wrong_method.Status(), mhttp::Status::MethodNotAllowed, "method mismatch becomes 405");
    CheckEq(wrong_method.Header("Allow"), "DELETE, GET, HEAD, OPTIONS, PATCH, POST, PROPFIND, PUT"sv, "sorted Allow");
    auto missing = dispatch(runtime, router, "GET", "/missing");
    CheckEq(missing.Status(), mhttp::Status::NotFound, "missing route becomes 404");
    CheckEq(missing.Body(), "404 Not Found\n"sv, "default error representation");
    Check(missing.Header("Allow").empty(), "404 has no Allow field");
    auto extension = dispatch(runtime, router, "UNKNOWN", "/verbs");
    CheckEq(extension.Status(), mhttp::Status::MethodNotAllowed, "unknown method on existing path is 405");
}

/**
 * @brief Populate request-owned captures after encoded-segment matching and preserve path priority.
 */
auto router_paths() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start path routing runtime");
    mhttp::server::Router router;
    Check(router.Get("/users/:id/files/:name", [](auto& request, auto& response) {
        CheckEq(request.template PathParam<int>("id"), std::optional{ 42 }, "typed route capture");
        CheckEq(request.PathParam("name"), "a/b"sv, "encoded slash remains within one capture");
        CheckEq(request.Query().Get("x"), "one two"sv, "handler sees parsed query");
        Check(response.SetContent("captures", "text/plain").has_value(), "capture response");
    }).has_value(), "multiple captures");
    CheckEq(dispatch(runtime, router, "GET", "/users/42/files/a%2Fb?x=one+two").Body(), "captures"sv, "owned capture callback");
    Check(router.Get("/choose/:name", [](auto&, auto& response) {
        Check(response.SetContent("parameter", "text/plain").has_value(), "parameter response");
    }).has_value(), "parameter path");
    Check(router.Get("/choose/static", [](auto&, auto& response) {
        Check(response.SetContent("static", "text/plain").has_value(), "static response");
    }).has_value(), "static path registered later");
    Check(router.Get("/choose/static/", [](auto&, auto& response) {
        Check(response.SetContent("slash", "text/plain").has_value(), "trailing slash response");
    }).has_value(), "trailing slash path");
    CheckEq(dispatch(runtime, router, "GET", "/choose/static").Body(), "static"sv, "static wins");
    CheckEq(dispatch(runtime, router, "GET", "/choose/static/").Body(), "slash"sv, "trailing slash distinct");
    CheckEq(dispatch(runtime, router, "GET", "/choose/other").Body(), "parameter"sv, "parameter fallback");
    CheckEq(dispatch(runtime, router, "GET", "/choose/").Body(), "parameter"sv, "empty parameter allowed");
    Check(router.Get("/files/*", [](auto& request, auto& response) {
        Check(response.SetContent(std::string{ request.PathParam("*") }, "text/plain").has_value(), "wildcard response");
    }).has_value(), "wildcard path");
    CheckEq(dispatch(runtime, router, "GET", "/files/a/b%20c").Body(), "a/b c"sv, "decoded wildcard remainder");
    CheckEq(dispatch(runtime, router, "GET", "/users/42/files/a/b").Status(), mhttp::Status::NotFound, "extra segment rejected");
}

/**
 * @brief Prefer explicit HEAD routes, fall back to GET, and retain HEAD framing without body transmission.
 */
auto router_head() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start HEAD runtime");
    mhttp::server::Router router;
    Check(router.Get("/items/:id", [](auto&, auto& response) {
        Check(response.SetContent("from GET", "text/plain").has_value(), "GET representation");
    }).has_value(), "GET fallback path");
    auto response = dispatch(runtime, router, "HEAD", "/items/42");
    CheckEq(response.Body(), "from GET"sv, "implicit HEAD executes GET callback");
    auto head = mhttp::detail::serialize_response_head(mhttp::detail::ServerResponseAccess::Head(response),
        mhttp::Method::Head, { .body_bytes = response.Body().size() });
    if (Check(head.has_value(), "serialize selected HEAD representation")) {
        Check(!head->send_body, "HEAD suppresses body transmission");
        Check(head->bytes.contains("Content-Length: 8\r\n"), "HEAD advertises GET representation length");
    }
    CheckEq(dispatch(runtime, router, "POST", "/items/42").Header("Allow"), "GET, HEAD"sv, "implicit HEAD in Allow");
    Check(router.Head("/items/:id", [](auto&, auto& response) {
        Check(response.SetContent("explicit HEAD", "text/plain").has_value(), "explicit HEAD response");
    }).has_value(), "explicit HEAD path");
    CheckEq(dispatch(runtime, router, "HEAD", "/items/42").Body(), "explicit HEAD"sv, "explicit HEAD wins");
    CheckEq(dispatch(runtime, router, "GET", "/items/42").Body(), "from GET"sv, "GET binding retained");
}

/**
 * @brief Reject invalid registrations atomically and enforce configured regex admission and subject bounds.
 */
auto router_validation() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start validation runtime");
    mhttp::server::Router router{ { .regex_pattern_bytes = 8, .regex_subject_bytes = 3 } };
    auto handler = [](auto&, auto& response) {
        Check(response.SetContent("safe", "text/plain").has_value(), "safe regex response");
    };
    Check(router.Get("/n/{id:[0-9]+}", handler).has_value(), "safe regex registration");
    for (auto path : { "/n/{id:(a+)+}", "/n/{id:[}", "/n/{id:123456789}", "relative", "/n/:id/:id" }) {
        auto result = router.Get(path, handler);
        Check(!result && result.error() == mhttp::Error::InvalidRequest, "invalid pattern rejected");
    }
    auto invalid = router.Route(static_cast<mhttp::Method>(999), "/n/{id:[0-9]+}", handler);
    Check(!invalid && invalid.error() == mhttp::Error::InvalidRequest, "invalid method rejected");
    CheckEq(dispatch(runtime, router, "GET", "/n/123").Body(), "safe"sv, "prior route survives errors");
    CheckEq(dispatch(runtime, router, "GET", "/n/1234").Status(), mhttp::Status::NotFound, "regex subject bound enforced");
    CheckEq(dispatch(runtime, router, "GET", "/n/abc").Status(), mhttp::Status::NotFound, "regex non-match");
}

/**
 * @brief Probe callback destruction by reentering registration after the old binding is retired.
 */
struct RetirementProbe {
    mhttp::server::Router& router; ///< Router alive through replacement.
    bool& released; ///< Observation alive through replacement.

    /**
     * @brief Retain observations without creating an early temporary destructor.
     * @param owner Router borrowed until destruction.
     * @param observation Borrowed release flag.
     */
    RetirementProbe(mhttp::server::Router& owner, bool& observation) : router{ owner }, released{ observation } {}

    /**
     * @brief Register from user destruction code to detect a held configuration mutex.
     */
    ~RetirementProbe() {
        released = true;
        Check(router.Get("/retired", [](auto&, auto&) {}).has_value(), "callback destructor may reenter registration");
    }
};

/**
 * @brief Replace only the exact method binding and destroy unused user state outside the configuration lock.
 */
auto router_replacement() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start replacement runtime");
    mhttp::server::Router router;
    bool released{};
    auto probe = std::make_shared<RetirementProbe>(router, released);
    Check(router.Get("/same", [owned = std::move(probe)](auto&, auto&) {}).has_value(), "owned old handler");
    Check(!released, "configuration retains callback");
    Check(router.Post("/same", [](auto&, auto& response) {
        Check(response.SetContent("post", "text/plain").has_value(), "POST binding response");
    }).has_value(), "distinct method binding");
    Check(router.Get("/same", [](auto&, auto& response) {
        Check(response.SetContent("new", "text/plain").has_value(), "replacement response");
    }).has_value(), "replace GET binding");
    Check(released, "replacement releases old captures immediately");
    CheckEq(dispatch(runtime, router, "GET", "/same").Body(), "new"sv, "replacement selected");
    CheckEq(dispatch(runtime, router, "POST", "/same").Body(), "post"sv, "other method retained");
    CheckEq(dispatch(runtime, router, "GET", "/retired").Status(), mhttp::Status::Ok, "destructor registration completed");
}

/**
 * @brief Guard frozen configuration and lazy/prepared readers until their final ownership is released.
 */
auto router_guards() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start configuration runtime");
    mhttp::server::Router router;
    auto request = ServerRequestAccess::Create({ .target = "/items" });
    mhttp::server::Response response;
    auto handler = [](auto&, auto&) {};
    Check(router.Get("/items", handler).has_value(), "initial registration");
    {
        auto pending = RouterAccess::Prepare(router, request, response);
        auto result = router.Get("/later", handler);
        Check(!result && result.error() == mhttp::Error::Busy, "unstarted Task prevents mutation");
        auto frozen = RouterAccess::Freeze(router);
        Check(!frozen && frozen.error() == mhttp::Error::Busy, "unstarted Task prevents freezing");
    }
    Check(router.Get("/later", handler).has_value(), "dropped Task releases reader");
    Check(RouterAccess::Freeze(router).has_value(), "freeze before server startup");
    auto repeated = RouterAccess::Freeze(router);
    Check(!repeated && repeated.error() == mhttp::Error::AlreadyRunning, "duplicate freeze rejected");
    auto rejected = router.Get("/blocked", handler);
    Check(!rejected && rejected.error() == mhttp::Error::AlreadyRunning, "frozen mutation rejected");
    {
        auto prepared = mhttp::SyncWait(runtime, RouterAccess::Prepare(router, request, response));
        Check(request.PathParam("id").empty(), "capture state initialized");
        auto thawed = RouterAccess::Thaw(router);
        Check(!thawed && thawed.error() == mhttp::Error::Busy, "prepared response write lease prevents thaw");
        mhttp::SyncWait(runtime, RouterAccess::Process(prepared, request, response));
        auto copy = prepared;
        prepared = {};
        Check(!RouterAccess::Thaw(router), "every prepared copy must drain");
    }
    Check(RouterAccess::Thaw(router).has_value(), "thaw after all reader copies drain");
    Check(router.Get("/blocked", handler).has_value(), "registration resumes after stop");
    Check(RouterAccess::Thaw(router).has_value(), "idle thaw idempotent");
}

/**
 * @brief Own suspended callbacks independently of the public wrapper and release all captures after completion.
 */
auto router_ownership() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start ownership runtime");
    auto request = ServerRequestAccess::Create({ .target = "/owned" });
    mhttp::server::Response response;
    auto router = std::make_unique<mhttp::server::Router>();
    auto owned = std::make_shared<int>(42);
    std::weak_ptr<int> weak{ owned };
    Check(router->Get("/owned", [owned = std::move(owned)](auto&, auto& output) -> mhttp::Task<void> {
        co_await mhttp::Sleep(1ms);
        CheckEq(*owned, 42, "callback capture survives wrapper destruction and suspension");
        Check(output.SetContent("owned", "text/plain").has_value(), "owned handler response");
    }).has_value(), "move-only lifetime registration");
    auto pending = RouterAccess::Dispatch(*router, request, response);
    router.reset();
    Check(!weak.expired(), "unstarted dispatch owns configuration");
    mhttp::SyncWait(runtime, std::move(pending));
    CheckEq(response.Body(), "owned"sv, "orphaned wrapper dispatch completes");
    Check(weak.expired(), "completed dispatch releases final callback captures");
    auto unused = std::make_unique<mhttp::server::Router>();
    auto value = std::make_shared<int>(7);
    std::weak_ptr<int> dropped{ value };
    Check(unused->Get("/owned", [value = std::move(value)](auto&, auto&) {}).has_value(), "unstarted lifetime route");
    {
        auto task = RouterAccess::Dispatch(*unused, request, response);
        unused.reset();
        Check(!dropped.expired(), "unstarted dispatch keeps handler");
    }
    Check(dropped.expired(), "abandoned lazy dispatch releases handler");
    static_assert(!std::is_copy_constructible_v<mhttp::server::Router>);
    static_assert(!std::is_move_constructible_v<mhttp::server::Router>);
    static_assert(std::has_virtual_destructor_v<mhttp::server::Router>);
}

/**
 * @brief Match immutable configuration concurrently and reject mutation during a suspended callback.
 */
auto router_concurrency() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start concurrent routing runtime");
    mhttp::server::Router router;
    std::latch entered{ 1 };
    std::atomic<bool> release{};
    std::atomic<unsigned> count{};
    Check(router.Get("/items/:id", [&entered, &release, &count](auto& request, auto& response) -> mhttp::Task<void> {
        if (count.fetch_add(1) == 0) {
            entered.count_down();
            while (!release.load()) { co_await mhttp::Sleep(1ms); }
        }
        co_await mhttp::Sleep(2ms);
        Check(response.SetContent(std::string{ request.PathParam("id") }, "text/plain").has_value(), "concurrent response");
    }).has_value(), "concurrent handler");
    std::vector<std::jthread> callers;
    for (unsigned index{}; index < 8; ++index) {
        callers.emplace_back([&, index] {
            for (unsigned iteration{}; iteration < 20; ++iteration) {
                auto id = std::to_string(index * 100 + iteration);
                CheckEq(dispatch(runtime, router, "GET", "/items/" + id).Body(), id, "concurrent captures remain request-local");
            }
        });
    }
    entered.wait();
    auto rejected = router.Get("/late", [](auto&, auto&) {});
    Check(!rejected && rejected.error() == mhttp::Error::Busy, "active request prevents concurrent mutation");
    release.store(true);
    callers.clear();
    CheckEq(count.load(), 160u, "all concurrent handlers completed");
    Check(router.Get("/late", [](auto&, auto&) {}).has_value(), "mutation resumes after concurrent dispatch drain");
}

/**
 * @brief Release reader leases on thrown callbacks and cancellation before allowing subsequent registration.
 */
auto router_failure_cleanup() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start failure routing runtime");
    mhttp::server::Router router;
    Check(router.Get("/throw", [](auto&, auto&) { throw 42; }).has_value(), "throwing route");
    auto failed = dispatch(runtime, router, "GET", "/throw");
    CheckEq(failed.Status(), mhttp::Status::InternalServerError, "arbitrary callback exceptions become 500");
    Check(router.Get("/after", [](auto&, auto&) {}).has_value(), "exception releases reader");
    std::latch entered{ 1 };
    Check(router.Get("/cancel", [&entered](auto&, auto&) -> mhttp::Task<void> {
        entered.count_down();
        co_await mhttp::Sleep(24h);
    }).has_value(), "suspended cancelable route");
    std::jthread stopper{ [&] { entered.wait(); runtime.Stop(); } };
    bool canceled{};
    try { static_cast<void>(dispatch(runtime, router, "GET", "/cancel")); }
    catch (std::system_error const& error) { canceled = error.code() == mhttp::Error::Canceled; }
    stopper.join();
    Check(canceled, "runtime cancellation reaches route boundary");
    Check(router.Get("/after-cancel", [](auto&, auto&) {}).has_value(), "cancellation releases reader");
    Check(runtime.Join().has_value(), "canceled routing runtime drains");
}

/**
 * @brief Run bounded public router scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "router method dispatch", router_methods },
        { "router encoded paths and captures", router_paths },
        { "router HEAD fallback and framing", router_head },
        { "router registration validation", router_validation },
        { "router callback replacement cleanup", router_replacement },
        { "router configuration guards", router_guards },
        { "router callback ownership", router_ownership },
        { "router concurrent dispatch", router_concurrency },
        { "router failure cleanup", router_failure_cleanup },
    });
}
