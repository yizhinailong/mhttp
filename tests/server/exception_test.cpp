/**
 * @file exception_test.cpp
 * @brief Verify contained routing failures, safe fallback output, exception hooks, cancellation, and concurrency.
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
 * @brief Return finalized output and connection policy together for exception boundary assertions.
 */
struct Output {
    mhttp::server::Response response; ///< Owned final application response.
    mhttp::detail::RoutingOutcome outcome; ///< Required connection and write policy.
};

/**
 * @brief Dispatch while retaining all borrowed request and response state through completion.
 * @param runtime Running executor.
 * @param router Configured routes.
 * @param target Encoded target.
 * @return Owned response and connection policy.
 */
auto dispatch(
    mhttp::Runtime& runtime,
    mhttp::server::Router& router,
    std::string target = "/throw"
) -> Output {
    auto request = ServerRequestAccess::Create({ .target = std::move(target) });
    mhttp::server::Response response;
    auto outcome = mhttp::SyncWait(runtime, RouterAccess::Dispatch(router, request, response));
    return { std::move(response), std::move(outcome) };
}

/**
 * @brief Convert standard, unknown, and asynchronous exceptions to safe 500 output and mandatory close.
 */
auto exception_defaults() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start exception runtime");
    for (unsigned scenario{}; scenario < 3; ++scenario) {
        mhttp::server::Router router;
        Check(router.set_http_handler<mhttp::method::Get>("/throw", [scenario](auto&, auto& response) -> mhttp::Task<void> {
            Check(response.Headers().Set("Location", "/partial").has_value(), "partial response field");
            Check(response.SetContent("partial secret", "text/plain").has_value(), "partial response body");
            co_await mhttp::Sleep(1ms);
            if (scenario == 0) { throw std::runtime_error{ "secret\r\nX-Injected: yes" }; }
            if (scenario == 1) { throw 42; }
            throw std::system_error{ mhttp::make_error_code(mhttp::Error::Read) };
        }).has_value(), "throwing asynchronous route");
        auto output = dispatch(runtime, router);
        CheckEq(output.response.Status(), mhttp::Status::InternalServerError, "default exception status");
        CheckEq(output.response.Body(), "500 Internal Server Error\n"sv, "default exception text contains no diagnostics");
        Check(output.response.Header("Location").empty(), "partial response fields are cleared");
        Check(output.outcome.close && output.outcome.send_response && output.outcome.failure, "send one 500 then close");
        auto head = mhttp::detail::serialize_response_head(mhttp::detail::ServerResponseAccess::Head(output.response),
            mhttp::Method::Get, { .body_bytes = output.response.Body().size(), .keep_alive = !output.outcome.close });
        if (Check(head.has_value(), "serialize safe exception response")) {
            Check(head->bytes.contains("Connection: close\r\n") && !head->keep_alive, "exception overrides persistence");
            Check(!head->bytes.contains("Injected") && !head->bytes.contains("secret"), "exception diagnostics stay out of headers");
        }
        Check(router.set_http_handler<mhttp::method::Get>("/ok", [](auto&, auto&) {}).has_value(), "exception releases routing lease");
        auto ok = dispatch(runtime, router, "/ok");
        CheckEq(ok.response.Status(), mhttp::Status::Ok, "subsequent requests remain usable");
        Check(!ok.outcome.close && ok.outcome.send_response && !ok.outcome.failure, "successful request policy independent");
    }
}

/**
 * @brief Deliver the original exception to an asynchronous customization hook, then run error/post once.
 */
auto exception_customization() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start custom exception runtime");
    mhttp::server::Router router;
    std::vector<std::string> trace;
    Check(router.set_http_handler<mhttp::method::Get>("/throw", [&](auto&, auto&) { trace.push_back("handler"); throw 42; }).has_value(), "unknown throwing handler");
    Check(router.SetExceptionHandler([&, value = std::make_unique<int>(7)](auto&, auto& response, std::exception_ptr failure) -> mhttp::Task<void> {
        trace.push_back("exception");
        co_await mhttp::Sleep(1ms);
        CheckEq(*value, 7, "move-only exception hook capture survives suspension");
        bool original{};
        try { std::rethrow_exception(failure); }
        catch (int number) { original = number == 42; }
        Check(original, "exception hook receives original unknown exception");
        CheckEq(response.Status(), mhttp::Status::InternalServerError, "safe default installed before customization");
        Check(response.SetStatus(mhttp::Status::BadRequest).has_value(), "custom exception status");
        Check(response.SetContent("customized", "text/plain").has_value(), "custom exception body");
        Check(response.Headers().Set("Connection", "keep-alive").has_value(), "application persistence preference");
    }).has_value(), "async exception hook");
    Check(router.SetErrorHandler([&](auto&, auto& response) {
        trace.push_back("error");
        CheckEq(response.Status(), mhttp::Status::BadRequest, "error hook sees customized status");
    }).has_value(), "error hook after exception");
    Check(router.SetPostRoutingHandler([&](auto&, auto& response) {
        trace.push_back("post");
        Check(response.Headers().Add("X-Post", "once").has_value(), "post after exception");
    }).has_value(), "post hook after exception");
    auto output = dispatch(runtime, router);
    CheckEq(trace, std::vector<std::string>{ "handler", "exception", "error", "post" }, "exception recovery phase order");
    CheckEq(output.response.Body(), "customized"sv, "exception customization retained");
    Check(output.outcome.close && output.outcome.send_response, "custom exception status still requires close");
    auto head = mhttp::detail::serialize_response_head(mhttp::detail::ServerResponseAccess::Head(output.response),
        mhttp::Method::Get, { .body_bytes = output.response.Body().size(), .keep_alive = !output.outcome.close });
    Check(head && !head->keep_alive && head->bytes.contains("Connection: close\r\n"), "close policy overrides hook header");
}

/**
 * @brief Defer pre-routing failure to response processing while preventing body intake and keeping a reader lease.
 */
auto exception_preparation() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start preparation exception runtime");
    mhttp::server::Router router;
    unsigned exceptions{};
    unsigned posts{};
    Check(router.SetPreRoutingHandler([](auto&, auto&) -> mhttp::Task<bool> {
        co_await mhttp::Sleep(1ms);
        throw std::range_error{ "pre-routing" };
        co_return false;
    }).has_value(), "throwing pre-routing hook");
    Check(router.SetExceptionHandler([&](auto&, auto&, std::exception_ptr failure) {
        ++exceptions;
        bool expected{};
        try { std::rethrow_exception(failure); }
        catch (std::range_error const& error) { expected = error.what() == "pre-routing"sv; }
        Check(expected, "pre-routing exception delivered unchanged");
    }).has_value(), "pre-routing exception observer");
    Check(router.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "pre-routing exception post hook");
    auto request = ServerRequestAccess::Create({ .method = "POST", .target = "/upload" });
    mhttp::server::Response response;
    auto prepared = mhttp::SyncWait(runtime, RouterAccess::Prepare(router, request, response));
    Check(prepared.handled && prepared.failure && !prepared.handler, "failed preparation skips route and body consumption");
    CheckEq(exceptions, 0u, "exception customization waits for processing");
    auto outcome = mhttp::SyncWait(runtime, RouterAccess::Process(prepared, request, response));
    Check(outcome.close && outcome.send_response, "pre-routing failure requires close");
    CheckEq(response.Status(), mhttp::Status::InternalServerError, "pre-routing safe default");
    CheckEq(exceptions, 1u, "one exception customization");
    CheckEq(posts, 1u, "one final response hook");
    auto mutation = router.set_http_handler<mhttp::method::Get>("/late", [](auto&, auto&) {});
    Check(!mutation && mutation.error() == mhttp::Error::Busy, "preparation failure retains response write lease");
}

/**
 * @brief Contain failures of exception/error handlers without recursion and still finalize a safe response once.
 */
auto exception_nested_failures() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start nested exception runtime");
    for (unsigned scenario{}; scenario < 3; ++scenario) {
        mhttp::server::Router router;
        unsigned exceptions{};
        unsigned errors{};
        unsigned posts{};
        Check(router.set_http_handler<mhttp::method::Get>("/throw", [scenario](auto&, auto& response) {
            if (scenario != 1) { throw 42; }
            Check(response.SetError(mhttp::Status::Forbidden).has_value(), "ordinary error response");
        }).has_value(), "nested failure route");
        Check(router.SetExceptionHandler([&](auto&, auto& response, std::exception_ptr) {
            ++exceptions;
            Check(response.Headers().Set("X-Partial", "secret").has_value(), "partial exception hook field");
            if (scenario == 0) { throw 7; }
        }).has_value(), "nested exception callback");
        Check(router.SetErrorHandler([&](auto&, auto&) {
            ++errors;
            if (scenario != 0) { throw 9; }
        }).has_value(), "nested error callback");
        Check(router.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "nested failure post callback");
        auto output = dispatch(runtime, router);
        CheckEq(output.response.Status(), mhttp::Status::InternalServerError, "nested failures fall back to 500");
        CheckEq(output.response.Body(), "500 Internal Server Error\n"sv, "nested failure safe body");
        Check(output.outcome.close && output.outcome.send_response, "nested failure closes after final response");
        CheckEq(exceptions, 1u, "exception hook never recurses or runs twice");
        CheckEq(errors, 1u, "error hook never recurses");
        CheckEq(posts, 1u, "nested failures still finalize once");
        if (scenario != 1) { Check(output.response.Header("X-Partial").empty(), "failed customization clears partial fields"); }
    }
}

/**
 * @brief A failed post hook forbids a partial response and cannot affect the next request's routing.
 */
auto exception_post_routing() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start post failure runtime");
    mhttp::server::Router router;
    unsigned posts{};
    unsigned exceptions{};
    Check(router.set_http_handler<mhttp::method::Get>("/throw", [](auto&, auto&) {}).has_value(), "post failure route");
    Check(router.set_http_handler<mhttp::method::Get>("/ok", [](auto&, auto&) {}).has_value(), "unrelated successful route");
    Check(router.SetExceptionHandler([&](auto&, auto&, std::exception_ptr) { ++exceptions; }).has_value(), "post failure exception observer");
    Check(router.SetPostRoutingHandler([&](auto& request, auto&) {
        ++posts;
        if (request.Path() == "/throw") { throw std::runtime_error{ "post failed" }; }
    }).has_value(), "throwing post hook");
    auto failed = dispatch(runtime, router);
    Check(failed.outcome.close && !failed.outcome.send_response && failed.outcome.failure, "post failure closes without writing partial output");
    CheckEq(posts, 1u, "post hook not repeated after failure");
    CheckEq(exceptions, 0u, "post failure skips exception customization");
    auto ok = dispatch(runtime, router, "/ok");
    Check(!ok.outcome.close && ok.outcome.send_response, "post failure does not poison next request");
    CheckEq(posts, 2u, "subsequent request finalizes normally");
}

/**
 * @brief Run 1,000 concurrent exception/customization/error sequences without sharing per-request state.
 */
auto exception_concurrency() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start concurrent exception runtime");
    mhttp::server::Router router;
    std::atomic<unsigned> exceptions{};
    std::atomic<unsigned> errors{};
    Check(router.set_http_handler<mhttp::method::Get>("/throw/:id", [](auto& request, auto&) { throw std::string{ request.PathParam("id") }; }).has_value(), "concurrent throwing route");
    Check(router.SetExceptionHandler([&](auto& request, auto& response, std::exception_ptr failure) -> mhttp::Task<void> {
        ++exceptions;
        co_await mhttp::Sleep(1ms);
        std::string value;
        try { std::rethrow_exception(failure); }
        catch (std::string const& original) { value = original; }
        CheckEq(value, request.PathParam("id"), "exception belongs to its own request");
        Check(response.SetContent(value, "text/plain").has_value(), "concurrent custom exception body");
    }).has_value(), "concurrent exception hook");
    Check(router.SetErrorHandler([&](auto&, auto&) { ++errors; }).has_value(), "concurrent error hook");
    std::vector<std::jthread> callers;
    for (unsigned client{}; client < 10; ++client) {
        callers.emplace_back([&, client] {
            for (unsigned iteration{}; iteration < 100; ++iteration) {
                auto id = std::to_string(client * 100 + iteration);
                auto output = dispatch(runtime, router, "/throw/" + id);
                CheckEq(output.response.Body(), id, "concurrent error responses remain independent");
                Check(output.outcome.close && output.outcome.send_response, "concurrent failure connection policy");
            }
        });
    }
    callers.clear();
    CheckEq(exceptions.load(), 1000u, "all exception hooks completed");
    CheckEq(errors.load(), 1000u, "all error hooks completed");
    Check(router.set_http_handler<mhttp::method::Get>("/after", [](auto&, auto&) {}).has_value(), "concurrent failures drain every lease");
}

/**
 * @brief Retain exception callback captures before lazy dispatch and direct adapter Tasks first run.
 */
auto exception_ownership() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start exception ownership runtime");
    auto router = std::make_unique<mhttp::server::Router>();
    auto value = std::make_shared<int>(42);
    std::weak_ptr<int> weak{ value };
    auto callback = [value = std::move(value)](auto&, auto&, std::exception_ptr failure) -> mhttp::Task<void> {
        co_await mhttp::Sleep(1ms);
        CheckEq(*value, 42, "exception callback capture survives suspension");
        bool caught{};
        try { std::rethrow_exception(failure); }
        catch (int number) { caught = number == 42; }
        Check(caught, "exception pointer survives initial suspension");
    };
    auto adapter = mhttp::detail::make_http_exception_handler(std::move(callback));
    auto request = ServerRequestAccess::Create({ .target = "/throw" });
    mhttp::server::Response response;
    auto direct = adapter(request, response, std::make_exception_ptr(42));
    adapter = {};
    Check(!weak.expired(), "direct lazy exception Task owns callback");
    mhttp::SyncWait(runtime, std::move(direct));
    Check(weak.expired(), "direct exception Task releases callback");
    auto owned = std::make_shared<int>(7);
    std::weak_ptr<int> router_weak{ owned };
    Check(router->set_http_handler<mhttp::method::Get>("/throw", [](auto&, auto&) { throw 42; }).has_value(), "owned exception route");
    Check(router->SetExceptionHandler([owned = std::move(owned)](auto&, auto&, std::exception_ptr) -> mhttp::Task<void> {
        co_await mhttp::Sleep(1ms);
        CheckEq(*owned, 7, "orphaned router exception state remains alive");
    }).has_value(), "owned exception callback");
    auto pending = RouterAccess::Dispatch(*router, request, response);
    router.reset();
    Check(!router_weak.expired(), "pending dispatch owns exception hook");
    auto outcome = mhttp::SyncWait(runtime, std::move(pending));
    Check(outcome.close && outcome.send_response, "orphaned router completes exception customization");
    Check(router_weak.expired(), "completed dispatch releases exception hook");
    static_assert(!mhttp::detail::HttpExceptionFunction<int>);
    static_assert(!mhttp::detail::HttpExceptionFunction<decltype([](auto&, auto&, std::exception_ptr) { return true; })>);
}

/**
 * @brief Keep cancellation from pre/error/exception/post hooks as control flow while releasing every reader.
 */
auto exception_cancellation() -> void {
    for (unsigned scenario{}; scenario < 4; ++scenario) {
        mhttp::Runtime runtime;
        Check(runtime.Start(2).has_value(), "start canceled-hook runtime");
        mhttp::server::Router router;
        std::latch entered{ 1 };
        if (scenario == 0) {
            Check(router.SetPreRoutingHandler([&](auto&, auto&) -> mhttp::Task<bool> {
                entered.count_down();
                co_await mhttp::Sleep(24h);
                co_return false;
            }).has_value(), "cancelable pre-routing");
        } else if (scenario == 1) {
            Check(router.SetErrorHandler([&](auto&, auto&) -> mhttp::Task<void> {
                entered.count_down();
                co_await mhttp::Sleep(24h);
            }).has_value(), "cancelable error hook");
        } else if (scenario == 2) {
            Check(router.set_http_handler<mhttp::method::Get>("/throw", [](auto&, auto&) { throw 42; }).has_value(), "cancelable exception source");
            Check(router.SetExceptionHandler([&](auto&, auto&, std::exception_ptr) -> mhttp::Task<void> {
                entered.count_down();
                co_await mhttp::Sleep(24h);
            }).has_value(), "cancelable exception hook");
        } else {
            Check(router.SetPostRoutingHandler([&](auto&, auto&) -> mhttp::Task<void> {
                entered.count_down();
                co_await mhttp::Sleep(24h);
            }).has_value(), "cancelable post-routing");
        }
        std::jthread stopper{ [&] { entered.wait(); runtime.Stop(); } };
        bool canceled{};
        try { static_cast<void>(dispatch(runtime, router)); }
        catch (std::system_error const& error) { canceled = error.code() == mhttp::Error::Canceled; }
        stopper.join();
        Check(canceled, "hook cancellation is propagated instead of converted to a response");
        Check(router.set_http_handler<mhttp::method::Get>("/after", [](auto&, auto&) {}).has_value(), "canceled hook releases configuration reader");
        Check(runtime.Join().has_value(), "canceled hook runtime drains");
    }
}

/**
 * @brief Run bounded exception scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "exception safe default responses", exception_defaults },
        { "exception asynchronous customization", exception_customization },
        { "exception before body intake", exception_preparation },
        { "exception failing recovery hooks", exception_nested_failures },
        { "exception post-routing isolation", exception_post_routing },
        { "exception concurrent customization", exception_concurrency },
        { "exception lazy callback ownership", exception_ownership },
        { "exception hook cancellation", exception_cancellation },
    });
}
