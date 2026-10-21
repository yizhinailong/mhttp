/**
 * @file test_server_test.cpp
 * @brief Verify the reusable public listener fixture owns setup and fully drains suspended handlers.
 */
import std;
import mhttp;

#include "support/test_support.hpp"
#include "support/test_server.hpp"
#include "support/thread_task.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Configure common routes before startup and observe bounded persistence over a dynamic port.
 */
auto fixture_requests() -> void {
    mhttp::test::TestServer fixture{ 1, { .keep_alive_max_requests = 2 } };
    Check(fixture.Server().Get("/request-target", [](auto& request, auto& response) {
        static_cast<void>(response.SetContent(std::string{ request.Target() }, "text/plain"));
    }).has_value(), "configure fixture routes before acceptance");
    auto const port = fixture.Start();
    Check(port != 0 && fixture.Server().IsRunning(), "fixture publishes an actual dynamic listener");
    mhttp::client::HttpClient client{ fixture.Execution(), "127.0.0.1", port };
    auto first = mhttp::SyncWait(fixture.Execution(), client.Get("/request-target?x=1"));
    Check(first && first->Body() == "/request-target?x=1" && first->KeepAlive(), "fixture serves owned target and persistence");
    auto second = mhttp::SyncWait(fixture.Execution(), client.Get("/request-target?x=2"));
    Check(second && second->Body() == "/request-target?x=2" && !second->KeepAlive(), "fixture honors configured request limit");
}

/**
 * @brief Count nested handler-frame cleanup before the fixture's runtime is destroyed.
 */
struct FixtureFrame {
    std::atomic<unsigned>& active; ///< Longer-lived observation counter.
    /**
     * @brief Publish construction.
     * @param count Borrowed counter retained through server teardown.
     */
    explicit FixtureFrame(std::atomic<unsigned>& count) : active{ count } { ++active; }
    /**
     * @brief Publish destruction after awaited work releases this frame.
     */
    ~FixtureFrame() { --active; }
};

/**
 * @brief Destroy a fixture during an accepted request and require handler cleanup before returning.
 */
auto fixture_teardown() -> void {
    mhttp::Runtime caller;
    Check(caller.Start(1).has_value(), "start independent fixture teardown caller");
    std::binary_semaphore entered{ 0 };
    std::atomic<unsigned> active{};
    std::unique_ptr<mhttp::client::HttpClient> client;
    std::unique_ptr<mhttp::test::ThreadTask<mhttp::Result<mhttp::client::Response>>> request;
    {
        mhttp::test::TestServer fixture;
        Check(fixture.Server().Get("/wait", [&](auto&, auto&) -> mhttp::Task<void> {
            FixtureFrame frame{ active };
            entered.release();
            co_await mhttp::Sleep(30s);
        }).has_value(), "register a suspended fixture request");
        client = std::make_unique<mhttp::client::HttpClient>(caller, "127.0.0.1", fixture.Start());
        request = std::make_unique<mhttp::test::ThreadTask<mhttp::Result<mhttp::client::Response>>>(
            [&] { return mhttp::SyncWait(caller, client->Get("/wait")); });
        Check(entered.try_acquire_for(1s) && active.load() == 1, "fixture handler begins before destruction");
    }
    CheckEq(active.load(), 0U, "fixture destructor joins the nested handler frame");
    Check(!request->Get(), "fixture destruction closes its active connection without a partial response");
}

/**
 * @brief Report invalid fixture configuration through the runner's exception path.
 */
auto fixture_setup_failure() -> void {
    bool rejected{};
    try { mhttp::test::TestServer invalid{ 0 }; }
    catch (std::runtime_error const&) { rejected = true; }
    Check(rejected, "invalid runtime thread count fails fixture setup without a blocked destructor");
}

/**
 * @brief Run fixture ownership and cleanup checks under the ordinary per-case watchdog.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Nonzero when any fixture behavior fails.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "test server requests", fixture_requests },
        { "test server suspended teardown", fixture_teardown },
        { "test server setup failure", fixture_setup_failure },
    });
}
