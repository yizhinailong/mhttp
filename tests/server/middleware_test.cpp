/**
 * @file middleware_test.cpp
 * @brief Verify global and route aspect ordering, short circuits, exception boundaries, and ownership.
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
 * @brief Record both phases and optionally short-circuit or throw at one phase.
 */
struct TraceAspect {
    std::vector<std::string>& trace; ///< Observation alive through dispatch.
    std::string name; ///< Phase prefix.
    bool before{ true }; ///< Continue Before decision.
    bool after{ true }; ///< Continue After decision.
    bool fail_before{}; ///< Throw instead of returning Before.
    bool fail_after{}; ///< Throw instead of returning After.

    /**
     * @brief Record entry after captures are populated and preserve a short-circuit response.
     * @param request Borrowed request.
     * @param response Borrowed mutable response.
     * @return Configured decision.
     */
    auto Before(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        trace.push_back(name + ".before");
        if (fail_before) { throw std::range_error{ name }; }
        if (request.Path() == "/items/42") {
            CheckEq(request.PathParam("id"), "42"sv, "global and route Before see path captures");
        }
        if (!before) { Check(response.SetError(mhttp::Status::Forbidden).has_value(), "short-circuit 403"); }
        return before;
    }

    /**
     * @brief Record exit in registration order, independently of which Before members ran.
     * @param request Unused borrowed request.
     * @param response Unused borrowed response.
     * @return Configured decision.
     */
    auto After(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        trace.push_back(name + ".after");
        if (fail_after) { throw std::range_error{ name }; }
        return after;
    }
};

/**
 * @brief Dispatch the configured path while keeping input and output alive through completion.
 * @param runtime Running executor.
 * @param router Configured router.
 * @param path Encoded target, defaulting to the traced route.
 * @return Owned response.
 */
auto run(
    mhttp::Runtime& runtime,
    mhttp::server::Router& router,
    std::string path = "/items/42"
) -> mhttp::server::Response {
    auto request = ServerRequestAccess::Create({ .target = std::move(path) });
    mhttp::server::Response response;
    mhttp::SyncWait(runtime, RouterAccess::Dispatch(router, request, response));
    return response;
}

/**
 * @brief Apply global aspects to routes registered before or after Use, with both layers ordered forwards.
 */
auto middleware_order() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start middleware runtime");
    std::vector<std::string> trace;
    mhttp::server::Router router;
    Check(router.Get("/items/:id", [&trace](auto&, auto&) { trace.push_back("handler"); },
        TraceAspect{ trace, "route1" }, TraceAspect{ trace, "route2" }).has_value(), "route aspects");
    Check(router.Use(TraceAspect{ trace, "global1" }).has_value(), "first global aspect");
    Check(router.Use(TraceAspect{ trace, "global2" }).has_value(), "second global aspect");
    static_cast<void>(run(runtime, router));
    CheckEq(trace, std::vector<std::string>{ "global1.before", "global2.before", "route1.before", "route2.before",
        "handler", "route1.after", "route2.after", "global1.after", "global2.after" }, "both layers run in registration order");
    for (auto path : { "/missing", "/items/42/extra" }) {
        trace.clear();
        CheckEq(run(runtime, router, path).Status(), mhttp::Status::NotFound, "global aspects wrap default errors");
        CheckEq(trace, std::vector<std::string>{ "global1.before", "global2.before", "global1.after", "global2.after" },
            "missing route executes global aspects exactly once");
    }
}

/**
 * @brief Short-circuit each layer independently while preserving the response and enclosing After phase.
 */
auto middleware_short_circuit() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start short-circuit runtime");
    for (unsigned scenario{}; scenario < 4; ++scenario) {
        std::vector<std::string> trace;
        mhttp::server::Router router;
        Check(router.Use(TraceAspect{ trace, "g1", scenario != 0, scenario != 3 },
            TraceAspect{ trace, "g2" }).has_value(), "global short-circuit configuration");
        Check(router.Get("/items/:id", [&trace](auto&, auto&) { trace.push_back("handler"); },
            TraceAspect{ trace, "r1", scenario != 1, scenario != 2 }, TraceAspect{ trace, "r2" }).has_value(),
            "route short-circuit configuration");
        auto response = run(runtime, router);
        std::vector<std::string> expected;
        switch (scenario) {
        case 0: expected = { "g1.before", "g1.after", "g2.after" }; break;
        case 1: expected = { "g1.before", "g2.before", "r1.before", "r1.after", "r2.after", "g1.after", "g2.after" }; break;
        case 2: expected = { "g1.before", "g2.before", "r1.before", "r2.before", "handler", "r1.after", "g1.after", "g2.after" }; break;
        case 3: expected = { "g1.before", "g2.before", "r1.before", "r2.before", "handler", "r1.after", "r2.after", "g1.after" }; break;
        }
        CheckEq(trace, expected, "false stops only its applicable chain");
        if (scenario < 2) {
            CheckEq(response.Status(), mhttp::Status::Forbidden, "Before short circuit preserves status");
            CheckEq(response.Body(), "403 Forbidden\n"sv, "Before short circuit preserves body");
        }
    }
}

/**
 * @brief Propagate exceptions from either layer without executing later callbacks or leaking a reader.
 */
auto middleware_exceptions() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start exception runtime");
    for (unsigned scenario{}; scenario < 5; ++scenario) {
        std::vector<std::string> trace;
        mhttp::server::Router router;
        Check(router.Use(TraceAspect{ trace, "g", true, true, scenario == 0, scenario == 4 }).has_value(), "throwing global configuration");
        Check(router.Get("/items/:id", [&](auto&, auto&) {
            trace.push_back("handler");
            if (scenario == 2) { throw std::range_error{ "handler" }; }
        }, TraceAspect{ trace, "r", true, true, scenario == 1, scenario == 3 }).has_value(), "throwing route configuration");
        bool caught{};
        try { static_cast<void>(run(runtime, router)); }
        catch (std::range_error const&) { caught = true; }
        Check(caught, "aspect/handler exception reaches boundary");
        std::vector<std::string> expected{ "g.before", "r.before", "handler", "r.after", "g.after" };
        expected.resize(scenario + 1);
        CheckEq(trace, expected, "exception skips all later stages including outer After");
        Check(router.Use(TraceAspect{ trace, "later" }).has_value(), "exception releases configuration reader");
    }
}

/**
 * @brief Own a move-only asynchronous aspect whose optional Before and After members share state.
 */
struct OwnedAspect {
    std::unique_ptr<int> value; ///< Move-only aspect resource.
    std::shared_ptr<int> observed; ///< Resource release observation.

    /**
     * @brief Yield before publishing context from owned state.
     * @param request Borrowed request.
     * @param response Unused borrowed response.
     * @return True.
     */
    auto Before(mhttp::server::Request& request, mhttp::server::Response& response) -> mhttp::Task<bool> {
        co_await mhttp::Sleep(1ms);
        request.Data().Store(*value);
        ++*value;
        co_return true;
    }

    /**
     * @brief Observe the same state after the route runs and an asynchronous suspension finishes.
     * @param request Unused borrowed request.
     * @param response Borrowed mutable response.
     * @return True.
     */
    auto After(mhttp::server::Request& request, mhttp::server::Response& response) -> mhttp::Task<bool> {
        co_await mhttp::Sleep(1ms);
        CheckEq(*value, 43, "Before and After share the same aspect");
        Check(response.Headers().Set("X-After", "yes").has_value(), "async global After response");
        co_return true;
    }
};

/**
 * @brief Retain global state through lazy suspension independently of Router and direct adapter lifetimes.
 */
auto middleware_ownership() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start ownership runtime");
    auto request = ServerRequestAccess::Create({ .target = "/owned" });
    mhttp::server::Response response;
    auto router = std::make_unique<mhttp::server::Router>();
    auto owned = std::make_shared<int>(7);
    std::weak_ptr<int> weak{ owned };
    Check(router->Use(OwnedAspect{ std::make_unique<int>(42), std::move(owned) }).has_value(), "move-only async global aspect");
    Check(router->Get("/owned", [](auto& input, auto&) {
        CheckEq(input.Data().template Fetch<int>(), std::optional{ 42 }, "route sees asynchronous global context");
    }).has_value(), "context observing route");
    auto pending = RouterAccess::Dispatch(*router, request, response);
    router.reset();
    Check(!weak.expired(), "lazy dispatch retains global aspect");
    mhttp::SyncWait(runtime, std::move(pending));
    CheckEq(response.Header("X-After"), "yes"sv, "global asynchronous After completed");
    Check(weak.expired(), "completed dispatch releases global state");
    auto resource = std::make_shared<int>(3);
    std::weak_ptr<int> adapter_weak{ resource };
    auto adapter = mhttp::detail::make_http_middleware(OwnedAspect{ std::make_unique<int>(42), std::move(resource) });
    auto decision = adapter.before(request, response);
    adapter = {};
    Check(!adapter_weak.expired(), "issued decision owns aspect before first await");
    Check(mhttp::SyncWait(runtime, std::move(decision)), "owned decision completes");
    Check(adapter_weak.expired(), "direct decision releases aspect");
}

/**
 * @brief Reject global registration with the same lifecycle guard as routes and preserve existing order.
 */
auto middleware_guards() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start middleware guard runtime");
    std::vector<std::string> trace;
    mhttp::server::Router router;
    auto request = ServerRequestAccess::Create({ .target = "/missing" });
    mhttp::server::Response response;
    Check(router.Use(TraceAspect{ trace, "first" }).has_value(), "initial middleware");
    {
        auto pending = RouterAccess::Prepare(router, request, response);
        auto result = router.Use(TraceAspect{ trace, "busy" });
        Check(!result && result.error() == mhttp::Error::Busy, "lazy reader guards Use");
    }
    Check(RouterAccess::Freeze(router).has_value(), "freeze middleware");
    auto result = router.Use(TraceAspect{ trace, "frozen" });
    Check(!result && result.error() == mhttp::Error::AlreadyRunning, "frozen router guards Use");
    static_cast<void>(run(runtime, router, "/missing"));
    CheckEq(trace, std::vector<std::string>{ "first.before", "first.after" }, "rejected additions leave middleware unchanged");
    Check(RouterAccess::Thaw(router).has_value(), "thaw middleware");
}

/**
 * @brief Run bounded middleware scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "middleware global route ordering", middleware_order },
        { "middleware independent short circuits", middleware_short_circuit },
        { "middleware exceptions skip later phases", middleware_exceptions },
        { "middleware asynchronous owned state", middleware_ownership },
        { "middleware configuration guards", middleware_guards },
    });
}
