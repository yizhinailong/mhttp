/**
 * @file routing_hooks_test.cpp
 * @brief Verify pre/post routing, custom 404 aspects, error transformations, and guarded callback ownership.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using mhttp::detail::RouterAccess;
using mhttp::detail::ServerRequestAccess;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

/**
 * @brief Record owned aspect entry and exit for route or custom 404 execution.
 */
struct TraceAspect {
    std::vector<std::string>& trace; ///< Observation alive through dispatch.
    std::string name; ///< Phase prefix.

    /**
     * @brief Record aspect entry.
     * @param request Unused borrowed request.
     * @param response Unused borrowed response.
     * @return True.
     */
    auto Before(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        trace.push_back(name + ".before");
        return true;
    }

    /**
     * @brief Record aspect exit.
     * @param request Unused borrowed request.
     * @param response Unused borrowed response.
     * @return True.
     */
    auto After(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        trace.push_back(name + ".after");
        return true;
    }
};

/**
 * @brief Execute in-process routing for an owned request.
 * @param runtime Running executor.
 * @param router Configured routes.
 * @param method Wire method.
 * @param target Encoded target.
 * @return Owned response.
 */
auto dispatch(
    mhttp::Runtime& runtime,
    mhttp::server::Router& router,
    std::string method,
    std::string target
) -> mhttp::server::Response {
    auto request = ServerRequestAccess::Create({ .method = std::move(method), .target = std::move(target) });
    mhttp::server::Response response;
    mhttp::SyncWait(runtime, RouterAccess::Dispatch(router, request, response));
    return response;
}

/**
 * @brief Run pre-routing before captures, then aspects and handler, error transformation, and one post hook.
 */
auto routing_hook_order() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start routing hook runtime");
    mhttp::server::Router router;
    std::vector<std::string> trace;
    Check(router.SetPreRoutingHandler([&](auto& request, auto&) {
        trace.push_back("pre");
        Check(request.PathParam("id").empty() && request.Body().empty(), "pre-routing precedes captures and body intake");
        return false;
    }).has_value(), "pre-routing hook");
    Check(router.Use(TraceAspect{ trace, "global" }).has_value(), "global aspect");
    Check(router.set_http_handler<mhttp::method::Get>("/items/:id", [&](auto& request, auto& response) {
        trace.push_back("handler");
        CheckEq(request.PathParam("id"), "42"sv, "handler captures after pre-routing");
        Check(response.SetError(mhttp::Status::Forbidden).has_value(), "handler error result");
    }, TraceAspect{ trace, "route" }).has_value(), "route and aspect");
    Check(router.SetErrorHandler([&](auto&, auto& response) {
        trace.push_back("error");
        CheckEq(response.Status(), mhttp::Status::Forbidden, "error hook sees final route status");
    }).has_value(), "error hook");
    Check(router.SetPostRoutingHandler([&](auto&, auto& response) {
        trace.push_back("post");
        Check(response.Headers().Add("X-Post", "once").has_value(), "post-routing response field");
    }).has_value(), "post-routing hook");
    auto response = dispatch(runtime, router, "GET", "/items/42");
    CheckEq(trace, std::vector<std::string>{ "pre", "global.before", "route.before", "handler", "route.after", "global.after", "error", "post" },
        "routing phases have a defined order");
    CheckEq(response.Headers().Values("X-Post"), std::vector{ "once"sv }, "one post-routing call");
}

/**
 * @brief Intercept before matching without invoking route/global aspects and still finalize errors and post once.
 */
auto routing_interception() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start interception runtime");
    mhttp::server::Router router;
    std::vector<std::string> trace;
    Check(router.Use(TraceAspect{ trace, "global" }).has_value(), "intercepted global aspect");
    Check(router.set_http_handler<mhttp::method::Get>("/intercepted", [&](auto&, auto&) { trace.push_back("handler"); }).has_value(), "intercepted route");
    Check(router.SetNotFoundHandler([&](auto&, auto&) { trace.push_back("404"); }).has_value(), "intercepted custom 404");
    Check(router.SetPreRoutingHandler([&](auto&, auto& response) -> mhttp::Task<bool> {
        trace.push_back("pre");
        co_await mhttp::Sleep(1ms);
        Check(response.SetError(mhttp::Status::NotFound).has_value(), "interception response");
        co_return true;
    }).has_value(), "asynchronous interception");
    Check(router.SetErrorHandler([&](auto&, auto&) { trace.push_back("error"); }).has_value(), "interception error hook");
    Check(router.SetPostRoutingHandler([&](auto&, auto&) { trace.push_back("post"); }).has_value(), "interception post hook");
    auto request = ServerRequestAccess::Create({ .method = "POST", .target = "/intercepted" });
    mhttp::server::Response response;
    auto prepared = mhttp::SyncWait(runtime, RouterAccess::Prepare(router, request, response));
    Check(prepared.handled && !prepared.match.handler && !prepared.handler, "handled preparation skips method and path selection");
    CheckEq(trace, std::vector<std::string>{ "pre" }, "interception runs during preparation before body intake");
    mhttp::SyncWait(runtime, RouterAccess::Process(prepared, request, response));
    CheckEq(response.Status(), mhttp::Status::NotFound, "interception preserves response");
    CheckEq(trace, std::vector<std::string>{ "pre", "error", "post" }, "interception skips routing but finalizes response");
}

/**
 * @brief Customize missing and handler-generated 404 responses inside globals while preserving 405 handling.
 */
auto routing_not_found() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start custom 404 runtime");
    mhttp::server::Router router;
    std::vector<std::string> trace;
    Check(router.Use(TraceAspect{ trace, "global" }).has_value(), "custom 404 globals");
    Check(router.SetNotFoundHandler([&](auto& request, auto& response) {
        trace.push_back("404");
        CheckEq(response.Status(), mhttp::Status::NotFound, "404 is initialized before custom handler");
        Check(response.SetContent(std::string{ request.Path() }, "text/plain").has_value(), "custom 404 representation");
    }, TraceAspect{ trace, "missing" }).has_value(), "404 handler with aspect");
    Check(router.set_http_handler<mhttp::method::Get>("/declared", [](auto&, auto& response) {
        Check(response.SetError(mhttp::Status::NotFound).has_value(), "handler-generated 404");
    }).has_value(), "route returning 404");
    Check(router.SetPostRoutingHandler([&](auto&, auto&) { trace.push_back("post"); }).has_value(), "404 post hook");
    for (auto path : { "/missing", "/declared" }) {
        trace.clear();
        auto response = dispatch(runtime, router, "GET", path);
        CheckEq(response.Status(), mhttp::Status::NotFound, "custom 404 preserves status");
        CheckEq(response.Body(), path, "custom handler sees request path");
        CheckEq(trace, std::vector<std::string>{ "global.before", "missing.before", "404", "missing.after", "global.after", "post" },
            "404 aspects stay inside one global chain");
    }
    trace.clear();
    auto mismatch = dispatch(runtime, router, "POST", "/declared");
    CheckEq(mismatch.Status(), mhttp::Status::MethodNotAllowed, "405 is not a missing route");
    CheckEq(mismatch.Header("Allow"), "GET, HEAD"sv, "405 retains method list");
    CheckEq(trace, std::vector<std::string>{ "global.before", "global.after", "post" }, "405 post-routing executes once");
}

/**
 * @brief Recompute framing after an asynchronous error handler changes status and content, without recursion.
 */
auto routing_error_framing() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start error transformation runtime");
    mhttp::server::Router router;
    unsigned errors{};
    unsigned posts{};
    Check(router.SetErrorHandler([&](auto&, auto& response) -> mhttp::Task<void> {
        ++errors;
        co_await mhttp::Sleep(1ms);
        Check(response.SetStatus(mhttp::Status::Ok).has_value(), "error handler may change status");
        Check(response.SetContent("changed", "text/plain").has_value(), "error handler may change body size");
        Check(response.Headers().Set("Content-Length", "999").has_value(), "stale user framing");
    }).has_value(), "async error handler");
    Check(router.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "framing post handler");
    auto response = dispatch(runtime, router, "GET", "/missing");
    CheckEq(response.Status(), mhttp::Status::Ok, "error transformation retained");
    auto serialized = mhttp::detail::serialize_response_head(mhttp::detail::ServerResponseAccess::Head(response),
        mhttp::Method::Get, { .body_bytes = response.Body().size() });
    if (Check(serialized.has_value(), "serialize final error result")) {
        Check(serialized->bytes.contains("Content-Length: 7\r\n"), "framing uses transformed body size");
        Check(!serialized->bytes.contains("999"), "stale length never reaches wire");
    }
    CheckEq(errors, 1u, "status change does not recursively invoke error handler");
    CheckEq(posts, 1u, "post runs once after error transformation");
    Check(router.set_http_handler<mhttp::method::Get>("/ok", [](auto&, auto&) {}).has_value(), "successful route");
    static_cast<void>(dispatch(runtime, router, "GET", "/ok"));
    CheckEq(errors, 1u, "successful responses skip error handler");
    CheckEq(posts, 2u, "successful response runs post");
}

/**
 * @brief Guard all hook setters and release retired callback resources outside the configuration mutex.
 */
auto routing_hook_guards() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start hook guard runtime");
    mhttp::server::Router router;
    std::vector<std::function<mhttp::Result<void>()>> setters{
        [&] { return router.SetNotFoundHandler([](auto&, auto&) {}); },
        [&] { return router.SetErrorHandler([](auto&, auto&) {}); },
        [&] { return router.SetExceptionHandler([](auto&, auto&, std::exception_ptr) {}); },
        [&] { return router.SetPreRoutingHandler([](auto&, auto&) { return false; }); },
        [&] { return router.SetPostRoutingHandler([](auto&, auto&) {}); },
    };
    auto request = ServerRequestAccess::Create({ .target = "/missing" });
    mhttp::server::Response response;
    {
        auto pending = RouterAccess::Prepare(router, request, response);
        for (auto& setter : setters) {
            auto result = setter();
            Check(!result && result.error() == mhttp::Error::Busy, "prepared request prevents hook mutation");
        }
    }
    Check(RouterAccess::Freeze(router).has_value(), "freeze hooks");
    for (auto& setter : setters) {
        auto result = setter();
        Check(!result && result.error() == mhttp::Error::AlreadyRunning, "frozen request prevents hook mutation");
    }
    Check(RouterAccess::Thaw(router).has_value(), "thaw hooks");
    for (auto& setter : setters) { Check(setter().has_value(), "idle hook mutation"); }
    auto owned = std::shared_ptr<int>{ new int{ 42 }, [&router](int* value) {
        delete value;
        Check(router.SetErrorHandler([](auto&, auto&) {}).has_value(), "retired hook destructor may reenter registration");
    } };
    std::weak_ptr<int> weak{ owned };
    Check(router.SetNotFoundHandler([owned = std::move(owned)](auto&, auto&) {}).has_value(), "own previous 404 callback");
    Check(router.SetNotFoundHandler([](auto&, auto&) {}).has_value(), "replace 404 callback");
    Check(weak.expired(), "replacement promptly retires old callback resources");
}

/**
 * @brief Keep move-only asynchronous pre/post closures alive through wrapper destruction and lazy dispatch.
 */
auto routing_hook_ownership() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start hook ownership runtime");
    auto router = std::make_unique<mhttp::server::Router>();
    auto owned = std::make_shared<int>(42);
    std::weak_ptr<int> weak{ owned };
    Check(router->SetPreRoutingHandler([value = std::make_unique<int>(7)](auto&, auto& response) -> mhttp::Task<bool> {
        co_await mhttp::Sleep(1ms);
        CheckEq(*value, 7, "move-only pre-routing capture survives suspension");
        Check(response.SetContent("intercepted", "text/plain").has_value(), "owned interception response");
        co_return true;
    }).has_value(), "move-only pre-routing callback");
    Check(router->SetPostRoutingHandler([owned = std::move(owned)](auto&, auto& response) -> mhttp::Task<void> {
        co_await mhttp::Sleep(1ms);
        CheckEq(*owned, 42, "post-routing capture survives suspension");
        Check(response.Headers().Set("X-Post", "owned").has_value(), "owned post-routing response");
    }).has_value(), "owned post-routing callback");
    auto request = ServerRequestAccess::Create({ .target = "/missing" });
    mhttp::server::Response response;
    auto pending = RouterAccess::Dispatch(*router, request, response);
    router.reset();
    Check(!weak.expired(), "lazy routing retains hooks");
    mhttp::SyncWait(runtime, std::move(pending));
    CheckEq(response.Body(), "intercepted"sv, "intercepted content retained");
    CheckEq(response.Header("X-Post"), "owned"sv, "owned post-routing completed");
    Check(weak.expired(), "completed routing releases hook resources");
    static_assert(!mhttp::detail::HttpDecisionFunction<int>);
    static_assert(!mhttp::detail::HttpDecisionFunction<decltype([](auto&, auto&) {})>);
}

/**
 * @brief Finalize parser errors without invoking interception or route aspects, retaining the write lease.
 */
auto routing_protocol_error() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start protocol error runtime");
    mhttp::server::Router router;
    std::vector<std::string> trace;
    Check(router.Use(TraceAspect{ trace, "global" }).has_value(), "protocol error globals");
    Check(router.SetPreRoutingHandler([&](auto&, auto&) { trace.push_back("pre"); return true; }).has_value(), "protocol error pre hook");
    Check(router.SetNotFoundHandler([&](auto&, auto&) { trace.push_back("404"); }).has_value(), "protocol error 404 hook");
    Check(router.SetErrorHandler([&](auto&, auto& response) {
        trace.push_back("error");
        CheckEq(response.Status(), mhttp::Status::RequestHeaderFieldsTooLarge, "error hook sees parser status");
    }).has_value(), "protocol error transformation");
    Check(router.SetPostRoutingHandler([&](auto&, auto&) { trace.push_back("post"); }).has_value(), "protocol error post hook");
    auto request = ServerRequestAccess::Create({});
    mhttp::server::Response response;
    Check(response.SetError(mhttp::Status::RequestHeaderFieldsTooLarge).has_value(), "prebuilt parser error");
    {
        auto prepared = RouterAccess::PrepareError(router);
        Check(prepared.handled && prepared.lifetime && !prepared.handler, "protocol error bypasses route selection");
        auto outcome = mhttp::SyncWait(runtime, RouterAccess::Process(prepared, request, response));
        Check(outcome.send_response && !outcome.failure, "protocol error response finalized");
        CheckEq(trace, std::vector<std::string>{ "error", "post" }, "only finalization hooks run for parser errors");
        auto blocked = router.set_http_handler<mhttp::method::Get>("/late", [](auto&, auto&) {});
        Check(!blocked && blocked.error() == mhttp::Error::Busy, "parser error write retains configuration guard");
    }
    Check(router.set_http_handler<mhttp::method::Get>("/late", [](auto&, auto&) {}).has_value(), "parser error lease releases after write");
}

/**
 * @brief Run bounded hook scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "routing hook phase order", routing_hook_order },
        { "routing early interception", routing_interception },
        { "routing custom 404 aspects", routing_not_found },
        { "routing error response framing", routing_error_framing },
        { "routing hook configuration guards", routing_hook_guards },
        { "routing hook lazy ownership", routing_hook_ownership },
        { "routing protocol error finalization", routing_protocol_error },
    });
}
