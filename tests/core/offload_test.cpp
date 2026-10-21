/**
 * @file offload_test.cpp
 * @brief Verify owned blocking work, strand restoration, responsive I/O, and stop/drain behavior.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::Task;
using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Observe a Task scheduler without suspending or exposing backend executor types.
 */
struct CurrentScheduler {
    std::shared_ptr<mhttp::detail::Scheduler> scheduler; ///< Scheduler retained until observation ends.

    /**
     * @brief Inspect the promise through the await_suspend protocol.
     * @return False.
     */
    auto await_ready() const noexcept -> bool { return false; }

    /**
     * @brief Borrow the currently executing Task's scheduler.
     * @tparam Promise Task promise type.
     * @param frame Currently running, caller-owned coroutine.
     * @return False, leaving the coroutine running inline.
     */
    template <typename Promise>
    auto await_suspend(std::coroutine_handle<Promise> frame) noexcept -> bool {
        scheduler = frame.promise().scheduler;
        return false;
    }

    /**
     * @brief Return the scheduler observed in await_suspend.
     * @return Retained scheduler identity.
     */
    auto await_resume() -> std::shared_ptr<mhttp::detail::Scheduler> { return std::move(scheduler); }
};

/**
 * @brief Test a move-only result without default construction or assignment.
 */
struct OwnedValue {
    std::unique_ptr<int> value; ///< Owned worker result.

    /**
     * @brief Adopt a value produced by a move-only callable.
     * @param input Owned scalar.
     */
    explicit OwnedValue(std::unique_ptr<int> input) : value{ std::move(input) } {}

    /**
     * @brief Move the worker result into its final owner.
     * @param other Previous owner.
     */
    OwnedValue(OwnedValue&& other) noexcept = default;

    /**
     * @brief Reject assignment so the bridge must construct results in place.
     * @param other Unused source.
     * @return Not callable.
     */
    auto operator=(OwnedValue&& other) -> OwnedValue& = delete;
};

/**
 * @brief Check void, move-only results/callables, exceptions, and original scheduler restoration.
 */
auto worker_results() -> Task<void> {
    auto original = co_await CurrentScheduler{};
    auto result = co_await mhttp::Offload([owned = std::make_unique<int>(42)]() mutable {
        Check(!mhttp::Runtime::IsIoThread(), "blocking callable runs outside every I/O thread");
        return OwnedValue{ std::move(owned) };
    });
    CheckEq(*result.value, 42, "move-only callable and result retain ownership");
    Check(mhttp::Runtime::IsIoThread(), "successful worker result resumes on an I/O thread");
    Check((co_await CurrentScheduler{}) == original, "success restores the original strand");
    bool invoked{};
    co_await mhttp::Offload([&invoked] { invoked = true; });
    Check(invoked, "void work completes before its continuation");
    bool caught{};
    try {
        co_await mhttp::Offload([]() -> int { throw std::domain_error{ "worker failure" }; });
    } catch (std::domain_error const& error) {
        caught = std::string_view{ error.what() } == "worker failure";
    }
    Check(caught, "worker exception keeps its original type and message");
    Check(mhttp::Runtime::IsIoThread(), "worker exception resumes on an I/O thread");
    Check((co_await CurrentScheduler{}) == original, "failure restores the original strand");
}

/**
 * @brief Run the value and exception cases on one I/O thread and separate workers.
 */
auto values_and_exceptions() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start worker value runtime");
    mhttp::SyncWait(runtime, worker_results());
    runtime.Stop();
    Check(runtime.Join().has_value(), "worker exceptions remain contained by Tasks");
}

/**
 * @brief Verify destroying an unstarted Offload releases its callable without invoking it.
 */
auto lazy_ownership() -> void {
    auto token = std::make_shared<int>(7);
    std::weak_ptr<int> weak = token;
    bool invoked{};
    {
        auto task = mhttp::Offload([owned = std::move(token), &invoked] { invoked = true; });
        Check(!weak.expired(), "unstarted Task owns its callable captures");
    }
    Check(!invoked, "unstarted blocking work stays lazy");
    Check(weak.expired(), "unstarted callable captures are released on destruction");
}

/**
 * @brief Verify a blocked worker leaves I/O responsive and drains before canceled frame destruction.
 */
auto stop_running_work() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start().has_value(), "start single I/O thread runtime");
    std::latch entered{ 1 };
    std::latch release{ 1 };
    std::atomic<bool> finished{};
    std::atomic<bool> canceled{};
    auto token = std::make_shared<int>(7);
    std::weak_ptr<int> weak = token;
    std::jthread waiter{ [&runtime, &entered, &release, &finished, &canceled, owned = std::move(token)]() mutable {
        try {
            mhttp::SyncWait(runtime, mhttp::Offload([&entered, &release, &finished, owned = std::move(owned)] {
                Check(!mhttp::Runtime::IsIoThread(), "blocked work is on a worker");
                entered.count_down();
                release.wait();
                CheckEq(*owned, 7, "worker captures remain alive through Stop");
                finished = true;
            }));
        } catch (std::system_error const& error) { canceled = error.code() == mhttp::Error::Canceled; }
    } };
    entered.wait();
    mhttp::SyncWait(runtime, mhttp::Sleep(5ms));
    auto const stopping = std::chrono::steady_clock::now();
    runtime.Stop();
    Check(std::chrono::steady_clock::now() - stopping < 100ms, "Stop does not wait for blocked work");
    Check(!finished, "Stop leaves the running callable intact");
    Check(!weak.expired(), "Stop retains the suspended Task and callable captures");
    std::atomic<bool> joined{};
    std::jthread joiner{ [&runtime, &joined] {
        Check(runtime.Join().has_value(), "Join drains the worker and its I/O continuation");
        joined = true;
    } };
    std::this_thread::sleep_for(10ms);
    Check(!joined, "Join waits for a running blocking function to finish");
    release.count_down();
    joiner.join();
    waiter.join();
    Check(finished && canceled, "running function finishes before cancellation is reported");
    Check(weak.expired(), "drain releases every worker capture");
    Check(runtime.Start().has_value(), "restart after worker drain");
    CheckEq(mhttp::SyncWait(runtime, mhttp::Offload([] { return 9; })), 9, "worker pool supports a new generation");
    runtime.Stop();
    Check(runtime.Join().has_value(), "join restarted worker runtime");
}

/**
 * @brief Verify concurrent roots share the bounded per-runtime pool.
 */
auto bounded_workers() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start multithreaded I/O runtime");
    std::atomic<unsigned> active{};
    std::atomic<unsigned> maximum{};
    std::vector<std::jthread> callers;
    for (unsigned i{}; i < 12; ++i) {
        callers.emplace_back([&runtime, &active, &maximum] {
            mhttp::SyncWait(runtime, mhttp::Offload([&active, &maximum] {
                auto const count = active.fetch_add(1) + 1;
                auto observed = maximum.load();
                while (observed < count && !maximum.compare_exchange_weak(observed, count)) {}
                std::this_thread::sleep_for(10ms);
                --active;
            }));
        });
    }
    callers.clear();
    Check(maximum > 0 && maximum <= 2, "concurrent roots share at most two blocking workers");
    CheckEq(active.load(), 0U, "every blocking function finishes before caller return");
    runtime.Stop();
    Check(runtime.Join().has_value(), "join bounded worker runtime");
}

/**
 * @brief Run independently filterable blocking-work cases.
 * @param argc Argument count.
 * @param argv Case name filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "values and exceptions", values_and_exceptions },
        { "lazy ownership", lazy_ownership },
        { "stop running work", stop_running_work },
        { "bounded workers", bounded_workers },
    });
}
