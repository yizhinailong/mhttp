/**
 * @file handler_test.cpp
 * @brief Verify lazy handler ownership, ordered aspects, short-circuit responses, and exception propagation.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

/**
 * @brief Record synchronous aspect ordering and configurable short-circuit decisions.
 */
struct TraceAspect {
    std::vector<std::string>& trace; ///< Observation alive until SyncWait completes.
    std::string name; ///< Aspect identity.
    bool before{ true }; ///< Before decision.
    bool after{ true }; ///< After decision.
    bool fail_before{}; ///< Throw during Before.
    bool fail_after{}; ///< Throw during After.

    /**
     * @brief Record entry and optionally publish a short-circuit response.
     * @param request Borrowed request with route captures already populated.
     * @param response Borrowed response to preserve when returning false.
     * @return Configured decision.
     */
    auto Before(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        trace.push_back(name + ".before");
        if (fail_before) { throw std::range_error{ "Before failed" }; }
        CheckEq(request.PathParam("id"), "42"sv, "Before sees route parameters");
        if (!before) { Check(response.SetError(mhttp::Status::Forbidden).has_value(), "short-circuit response"); }
        return before;
    }

    /**
     * @brief Record exit in registration order.
     * @param request Unused borrowed request.
     * @param response Unused borrowed response.
     * @return Configured decision.
     */
    auto After(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        trace.push_back(name + ".after");
        if (fail_after) { throw std::range_error{ "After failed" }; }
        return after;
    }
};

/**
 * @brief Provide an optional Before member without an After member.
 */
struct BeforeOnly {
    std::vector<std::string>& trace; ///< Borrowed observation.

    /**
     * @brief Record entry without supplying an After method.
     * @param request Unused request.
     * @param response Unused response.
     * @return True.
     */
    auto Before(mhttp::server::Request& request, mhttp::server::Response& response) -> bool {
        trace.push_back("only.before");
        return true;
    }
};

/**
 * @brief Provide an asynchronous After member without a Before member.
 */
struct AfterOnly {
    std::vector<std::string>& trace; ///< Borrowed observation.

    /**
     * @brief Yield then record exit without supplying a Before method.
     * @param request Unused request.
     * @param response Unused response.
     * @return True.
     */
    auto After(mhttp::server::Request& request, mhttp::server::Response& response) -> mhttp::Task<bool> {
        co_await mhttp::Sleep(1ms);
        trace.push_back("only.after");
        co_return true;
    }
};

/**
 * @brief Exercise Task<bool> aspect decisions across asynchronous suspension.
 */
struct AsyncAspect {
    std::vector<std::string>& trace; ///< Observation alive through completion.
    std::unique_ptr<int> value{ std::make_unique<int>(7) }; ///< Make the aspect move-only.
    bool before{ true }; ///< Before decision.

    /**
     * @brief Yield then store per-request data for the handler.
     * @param request Borrowed request.
     * @param response Borrowed response for short-circuit output.
     * @return Configured decision.
     */
    auto Before(mhttp::server::Request& request, mhttp::server::Response& response) -> mhttp::Task<bool> {
        trace.push_back("async.before");
        co_await mhttp::Sleep(1ms);
        request.Data().Store(*value);
        if (!before) { Check(response.SetError(mhttp::Status::Forbidden).has_value(), "async short circuit"); }
        co_return before;
    }

    /**
     * @brief Yield and record ordered exit.
     * @param request Unused borrowed request.
     * @param response Unused borrowed response.
     * @return True.
     */
    auto After(mhttp::server::Request& request, mhttp::server::Response& response) -> mhttp::Task<bool> {
        co_await mhttp::Sleep(1ms);
        trace.push_back("async.after");
        co_return true;
    }
};

/**
 * @brief Create initialized metadata and captures before invoking an adapter directly.
 * @return Owned fixture request.
 */
auto make_request() -> mhttp::server::Request {
    auto request = mhttp::detail::ServerRequestAccess::Create({ .target = "/items/42" });
    mhttp::QueryParams captures;
    captures.Add("id", "42");
    mhttp::detail::ServerRequestAccess::SetPathParams(request, std::move(captures));
    return request;
}

/**
 * @brief Mix synchronous and asynchronous callbacks while retaining context and registration order.
 */
auto handler_order() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start handler runtime");
    auto request = make_request();
    mhttp::server::Response response;
    std::vector<std::string> trace;
    auto handler = mhttp::detail::make_http_handler([&trace](auto& req, auto& resp) -> mhttp::Task<void> {
        trace.push_back("handler");
        CheckEq(req.Data().template Fetch<int>(), std::optional{ 7 }, "handler sees async middleware context");
        co_await mhttp::Sleep(1ms);
        Check(resp.SetContent("ok", "text/plain").has_value(), "handler response");
    }, TraceAspect{ trace, "first" }, AsyncAspect{ trace });
    auto task = handler(request, response);
    Check(trace.empty(), "callback chain is lazy");
    mhttp::SyncWait(runtime, std::move(task));
    CheckEq(trace, std::vector<std::string>{ "first.before", "async.before", "handler", "first.after", "async.after" },
        "Before and After both follow registration order");
    CheckEq(response.Body(), "ok"sv, "handler content retained");
    trace.clear();
    auto optional = mhttp::detail::make_http_handler([&trace](auto&, auto&) { trace.push_back("handler"); },
        BeforeOnly{ trace }, AfterOnly{ trace });
    mhttp::SyncWait(runtime, optional(request, response));
    CheckEq(trace, std::vector<std::string>{ "only.before", "handler", "only.after" }, "absent aspect members are skipped");
}

/**
 * @brief Preserve a Before response, run After even for skipped Before members, and stop an After chain on false.
 */
auto handler_short_circuit() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start short-circuit runtime");
    for (bool asynchronous : { false, true }) {
        auto request = make_request();
        mhttp::server::Response response;
        std::vector<std::string> trace;
        auto handler = mhttp::detail::make_http_handler([&trace](auto&, auto&) { trace.push_back("handler"); },
            TraceAspect{ trace, "first", asynchronous }, AsyncAspect{ trace, std::make_unique<int>(7), false });
        mhttp::SyncWait(runtime, handler(request, response));
        std::vector<std::string> expected{ "first.before" };
        if (asynchronous) { expected.push_back("async.before"); }
        expected.insert(expected.end(), { "first.after", "async.after" });
        CheckEq(trace, expected, "Before false skips later Before and handler, then runs all After");
        CheckEq(response.Status(), mhttp::Status::Forbidden, "short-circuit 403 preserved");
    }
    auto request = make_request();
    mhttp::server::Response response;
    std::vector<std::string> trace;
    auto handler = mhttp::detail::make_http_handler([&trace](auto&, auto&) { trace.push_back("handler"); },
        TraceAspect{ trace, "first", true, false }, TraceAspect{ trace, "second" });
    mhttp::SyncWait(runtime, handler(request, response));
    CheckEq(trace, std::vector<std::string>{ "first.before", "second.before", "handler", "first.after" },
        "After false skips later After members");
}

/**
 * @brief Retain move-only callback state before initial suspension, independently of the adapter object.
 */
auto handler_ownership() -> void {
    auto request = make_request();
    mhttp::server::Response response;
    std::weak_ptr<int> weak;
    {
        auto owned = std::make_shared<int>(42);
        weak = owned;
        auto handler = mhttp::detail::make_http_handler(
            [owned = std::move(owned), unique = std::make_unique<int>(1)](auto&, auto& resp) {
                Check(resp.SetContent(std::to_string(*owned + *unique), "text/plain").has_value(), "owned callback content");
            });
        {
            auto unstarted = handler(request, response);
            handler = {};
            Check(!weak.expired(), "unstarted Task retains callback state after adapter destruction");
        }
        Check(weak.expired(), "discarded Task releases callback captures");
    }
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start ownership runtime");
    auto owned = std::make_shared<int>(41);
    weak = owned;
    auto handler = mhttp::detail::make_http_handler([owned = std::move(owned)](auto&, auto& resp) -> mhttp::Task<void> {
        co_await mhttp::Sleep(1ms);
        Check(resp.SetContent(std::to_string(*owned), "text/plain").has_value(), "detached adapter callback executes");
    });
    auto pending = handler(request, response);
    handler = {};
    mhttp::SyncWait(runtime, std::move(pending));
    CheckEq(response.Body(), "41"sv, "Task executes independently of its adapter");
    Check(weak.expired(), "completed Task releases callback state before SyncWait returns");
}

/**
 * @brief Preserve standard and arbitrary exception types and skip all After members on handler failure.
 */
auto handler_exceptions() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start exception runtime");
    for (bool standard : { false, true }) {
        auto request = make_request();
        mhttp::server::Response response;
        std::vector<std::string> trace;
        auto handler = mhttp::detail::make_http_handler([&trace, standard](auto&, auto&) {
            trace.push_back("handler");
            if (standard) { throw std::range_error{ "private\r\nerror" }; }
            throw 42;
        }, TraceAspect{ trace, "first" });
        bool caught{};
        try { mhttp::SyncWait(runtime, handler(request, response)); }
        catch (std::range_error const& error) { caught = standard && std::string_view{ error.what() } == "private\r\nerror"; }
        catch (int error) { caught = !standard && error == 42; }
        Check(caught, "handler exception retains its type and content");
        CheckEq(trace, std::vector<std::string>{ "first.before", "handler" }, "handler exception skips After");
    }
    for (bool before : { false, true }) {
        auto request = make_request();
        mhttp::server::Response response;
        std::vector<std::string> trace;
        auto handler = mhttp::detail::make_http_handler([&trace](auto&, auto&) { trace.push_back("handler"); },
            TraceAspect{ trace, "first", true, true, before, !before }, TraceAspect{ trace, "second" });
        bool caught{};
        try { mhttp::SyncWait(runtime, handler(request, response)); }
        catch (std::range_error const& error) {
            caught = std::string_view{ error.what() } == (before ? "Before failed" : "After failed");
        }
        Check(caught, "aspect exception propagates unchanged");
        auto expected = before ? std::vector<std::string>{ "first.before" }
            : std::vector<std::string>{ "first.before", "second.before", "handler", "first.after" };
        CheckEq(trace, expected, "aspect exception skips all later stages");
    }
    static_assert(!mhttp::detail::HttpHandlerFunction<int>);
    static_assert(!mhttp::detail::HttpHandlerFunction<decltype([](auto&, auto&) { return 1; })>);
    static_assert(!mhttp::detail::HttpAspect<int>);
}

/**
 * @brief Cancel a handler suspended in a runtime operation and verify callbacks drain before returning.
 */
auto handler_cancellation() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start canceled-handler runtime");
    auto request = make_request();
    mhttp::server::Response response;
    std::vector<std::string> trace;
    std::latch entered{ 1 };
    auto owned = std::make_shared<int>(42);
    std::weak_ptr<int> weak{ owned };
    auto handler = mhttp::detail::make_http_handler(
        [owned = std::move(owned), &entered](auto&, auto&) -> mhttp::Task<void> {
            entered.count_down();
            co_await mhttp::Sleep(24h);
            Check(false, "canceled handler must not reach normal completion");
        }, TraceAspect{ trace, "first" });
    auto pending = handler(request, response);
    handler = {};
    std::jthread stopper{ [&] { entered.wait(); runtime.Stop(); } };
    bool canceled{};
    try { mhttp::SyncWait(runtime, std::move(pending)); }
    catch (std::system_error const& error) { canceled = error.code() == mhttp::Error::Canceled; }
    stopper.join();
    Check(canceled, "runtime cancellation reaches handler through its Task scheduler");
    CheckEq(trace, std::vector<std::string>{ "first.before" }, "cancellation skips After");
    Check(weak.expired(), "canceled handler releases owned captures before SyncWait returns");
    Check(runtime.Join().has_value(), "canceled handler runtime drains");
}

/**
 * @brief Run bounded handler-adaptation scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "handler callback ordering", handler_order },
        { "handler short circuit", handler_short_circuit },
        { "handler callback ownership", handler_ownership },
        { "handler exception propagation", handler_exceptions },
        { "handler cancellation cleanup", handler_cancellation },
    });
}
