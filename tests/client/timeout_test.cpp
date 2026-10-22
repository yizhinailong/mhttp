/**
 * @file timeout_test.cpp
 * @brief Verify shared redirect budgets and cancellation when automatic client deadlines are disabled.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Carry one overall deadline across new connections while Phase and Step permit each short hop.
 */
auto redirect_deadlines() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start redirect budget runtime");
    mhttp::server::HttpServer first{ runtime }, second{ runtime };
    auto first_port = first.BindToAnyPort("127.0.0.1");
    auto second_port = second.BindToAnyPort("127.0.0.1");
    Check(first_port && second_port, "bind redirect budget peers");
    Check(first.Get("/one", [second_port](auto&, auto& response) -> mhttp::Task<void> {
        co_await mhttp::Sleep(100ms);
        Check(response.SetRedirect("http://127.0.0.1:" + std::to_string(*second_port) + "/two").has_value(), "first delayed redirect");
    }).has_value(), "register first budget hop");
    Check(second.Get("/two", [first_port](auto&, auto& response) -> mhttp::Task<void> {
        co_await mhttp::Sleep(100ms);
        Check(response.SetRedirect("http://127.0.0.1:" + std::to_string(*first_port) + "/three").has_value(), "second delayed redirect");
    }).has_value(), "register second budget hop");
    Check(first.Get("/three", [](auto&, auto& response) -> mhttp::Task<void> {
        co_await mhttp::Sleep(100ms);
        Check(response.SetContent("complete", "text/plain").has_value(), "final delayed response");
    }).has_value(), "register final budget hop");
    Check(first.Start() && second.Start() && first.WaitUntilReady() && second.WaitUntilReady(), "start budget listeners");
    for (auto policy : { mhttp::client::TimeoutPolicy::Overall, mhttp::client::TimeoutPolicy::Step,
                         mhttp::client::TimeoutPolicy::Phase, mhttp::client::TimeoutPolicy::Never }) {
        // The overall deadline expires mid-way through the third hop's read, clear of the reconnect before it.
        mhttp::client::ClientOptions options{ .read_timeout = 200ms, .max_redirects = 2,
            .timeout_policy = policy, .timeout = 250ms };
        if (policy == mhttp::client::TimeoutPolicy::Phase) { options.timeout = 1ms; }
        if (policy == mhttp::client::TimeoutPolicy::Never) {
            options.connect_timeout = 1ms;
            options.read_timeout = 1ms;
            options.write_timeout = 1ms;
            options.timeout = 1ms;
        }
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", *first_port, options };
        auto const started = std::chrono::steady_clock::now();
        auto response = mhttp::SyncWait(runtime, client.Get("/one"));
        auto const elapsed = std::chrono::steady_clock::now() - started;
        if (policy == mhttp::client::TimeoutPolicy::Overall) {
            Check(!response && response.error() == mhttp::Error::ReadTimeout && !client.IsOpen(), "one overall deadline spans three hops and two origin changes");
            Check(elapsed >= 240ms && elapsed < 400ms, "redirect connections do not reset the overall budget");
        } else {
            Check(response && response->Body() == "complete" && elapsed >= 290ms, "Phase and Step restart their own budgets; Never waits for completion");
        }
    }
}

/**
 * @brief Cancel an untimed read through both client Stop and runtime Stop without waiting for peer progress.
 */
auto untimed_cancellation() -> void {
    mhttp::Runtime server_runtime;
    Check(server_runtime.Start(4).has_value(), "start untimed cancellation peer runtime");
    std::counting_semaphore<2> entered{ 0 };
    mhttp::server::HttpServer server{ server_runtime };
    Check(server.Get("/", [&entered](auto&, auto&) -> mhttp::Task<void> {
        entered.release();
        co_await mhttp::Sleep(5s);
    }).has_value(), "register untimed pending handler");
    auto port = server.BindToAnyPort("127.0.0.1");
    Check(port && server.Start() && server.WaitUntilReady(), "start untimed cancellation listener");
    for (bool stop_runtime : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start untimed client runtime");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port,
            { .read_timeout = 1ms, .timeout_policy = mhttp::client::TimeoutPolicy::Never, .timeout = 1ms } };
        std::optional<mhttp::Result<mhttp::client::Response>> result;
        std::exception_ptr failure;
        std::jthread waiter{ [&] {
            try { result.emplace(mhttp::SyncWait(runtime, client.Get())); }
            catch (...) { failure = std::current_exception(); }
        } };
        Check(entered.try_acquire_for(1s), "untimed request reaches peer");
        auto const started = std::chrono::steady_clock::now();
        if (stop_runtime) { runtime.Stop(); }
        else { client.Stop(); }
        waiter.join();
        if (failure) { std::rethrow_exception(failure); }
        Check(result && !*result && result->error() == mhttp::Error::Canceled && !client.IsOpen(), "Never preserves explicit and runtime cancellation");
        runtime.Stop();
        Check(runtime.Join().has_value(), "untimed canceled read drains before Join");
        Check(std::chrono::steady_clock::now() - started < 1s, "untimed cancellation and cleanup do not wait for handler timeout");
    }
}

/**
 * @brief Run bounded public timeout-policy acceptance tests.
 * @param argc Argument count.
 * @param argv Optional test filters.
 * @return Test process status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client redirect deadline policies", redirect_deadlines },
        { "client untimed cancellation", untimed_cancellation },
    });
}
