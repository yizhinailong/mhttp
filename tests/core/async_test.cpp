/**
 * @file async_test.cpp
 * @brief Verify runtime Task submission, timer cancellation, result cleanup, and I/O wait rejection.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::Task;
using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Exercise a result that cannot be default constructed, copied, or assigned.
 */
struct OwnedValue {
    int value; ///< Moved owned scalar.

    /**
     * @brief Construct from an explicit scalar.
     * @param input Scalar to own.
     */
    explicit OwnedValue(int input) : value{ input } {}

    /**
     * @brief Permit construction of the returned result.
     * @param other Value moved into the new owner.
     */
    OwnedValue(OwnedValue&& other) noexcept = default;

    /**
     * @brief Reject copying the returned result.
     * @param other Unused source.
     */
    OwnedValue(OwnedValue const& other) = delete;

    /**
     * @brief Require construction instead of assignment in result storage.
     * @param other Unused source.
     * @return Not callable.
     */
    auto operator=(OwnedValue&& other) -> OwnedValue& = delete;
};

/**
 * @brief Return a nondefault, move-only result across nested timers.
 * @return Owned result.
 */
auto owned_result() -> Task<OwnedValue> {
    Check(mhttp::Runtime::IsIoThread(), "Task starts on a runtime I/O thread");
    co_await mhttp::Sleep(0ms);
    Check(mhttp::Runtime::IsIoThread(), "timer completion resumes on an I/O thread");
    co_return OwnedValue{ 42 };
}

/**
 * @brief Throw after an asynchronous child completes.
 * @return Never returns normally.
 */
auto asynchronous_failure() -> Task<int> {
    co_await mhttp::Sleep(0ms);
    throw std::runtime_error{ "asynchronous failure" };
    co_return 0;
}

/**
 * @brief Verify type erasure preserves value and exception semantics for SyncWait.
 */
auto values_and_failures() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(3).has_value(), "start value runtime");
    auto result = mhttp::SyncWait(runtime, owned_result());
    CheckEq(result.value, 42, "nondefault result crosses the Task/Asio boundary");
    mhttp::SyncWait(runtime, mhttp::Sleep(-1ms));
    bool caught{};
    try { static_cast<void>(mhttp::SyncWait(runtime, asynchronous_failure())); }
    catch (std::runtime_error const& error) { caught = std::string_view{ error.what() } == "asynchronous failure"; }
    Check(caught, "original asynchronous user exception reaches SyncWait");
    bool empty{};
    try { mhttp::SyncWait(runtime, Task<void>{}); }
    catch (std::logic_error const&) { empty = true; }
    Check(empty, "empty Task fails through the completion path");
    runtime.Stop();
    Check(runtime.Join().has_value(), "user exceptions do not fail runtime callbacks");
}

/**
 * @brief Attempt blocking waits inside I/O execution, including a different Runtime.
 * @param runtime Owning runtime, alive until this Task completes.
 * @param other Another started runtime.
 */
auto reject_io_waits(mhttp::Runtime& runtime, mhttp::Runtime& other) -> Task<void> {
    for (auto* target : { &runtime, &other }) {
        bool rejected{};
        try { mhttp::SyncWait(*target, mhttp::Sleep(0ms)); }
        catch (std::system_error const& error) { rejected = error.code() == mhttp::Error::Busy; }
        Check(rejected, "SyncWait rejects blocking on every runtime from an I/O thread");
    }
    auto const joined = runtime.Join();
    Check(!joined && joined.error() == mhttp::Error::Busy, "Join rejects I/O execution");
    auto const run = other.Run();
    Check(!run && run.error() == mhttp::Error::Busy, "inline Run rejects I/O execution");
    co_return;
}

/**
 * @brief Verify synchronous wait errors are reported before any wait can deadlock.
 */
auto rejected_waits() -> void {
    mhttp::Runtime runtime;
    bool idle{};
    try { mhttp::SyncWait(runtime, mhttp::Sleep(0ms)); }
    catch (std::system_error const& error) { idle = error.code() == mhttp::Error::NotRunning; }
    Check(idle, "SyncWait rejects an idle runtime");
    mhttp::Runtime other;
    Check(runtime.Start(2).has_value() && other.Start().has_value(), "start both runtimes");
    mhttp::SyncWait(runtime, reject_io_waits(runtime, other));
    runtime.Stop();
    other.Stop();
    Check(runtime.Join().has_value() && other.Join().has_value(), "join both runtimes");
}

/**
 * @brief Verify a real timer delay and that owned coroutine parameters are released before return.
 * @param token Owned lifetime token observed outside the coroutine.
 */
auto delayed_owner(std::shared_ptr<int> token) -> Task<void> {
    co_await mhttp::Sleep(35ms);
    CheckEq(*token, 1, "parameter alive across timer suspension");
}

/**
 * @brief Verify Sleep timing and complete frame cleanup before SyncWait unblocks.
 */
auto timer_and_cleanup() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start timer runtime");
    auto token = std::make_shared<int>(1);
    std::weak_ptr<int> weak = token;
    auto const started = std::chrono::steady_clock::now();
    mhttp::SyncWait(runtime, delayed_owner(std::move(token)));
    auto const elapsed = std::chrono::steady_clock::now() - started;
    Check(elapsed >= 30ms && elapsed < 1s, "timer delay remains within scheduling tolerance");
    Check(weak.expired(), "SyncWait returns after destroying all user coroutine frames");
    runtime.Stop();
    Check(runtime.Join().has_value(), "join timer runtime");
}

/**
 * @brief Announce entry before waiting on a timer much longer than the stop deadline.
 * @param entered Borrowed readiness promise.
 * @param token Owned lifetime token.
 */
auto cancellable_sleep(std::promise<void>& entered, std::shared_ptr<int> token) -> Task<void> {
    entered.set_value();
    co_await mhttp::Sleep(10s);
    CheckEq(*token, 1, "unexpected uncanceled timer result remains valid");
}

/**
 * @brief Verify cancellation before/during operation initiation drains frames and allows restart.
 */
auto stop_pending_sleep() -> void {
    mhttp::Runtime runtime;
    for (unsigned i{}; i < 25; ++i) {
        Check(runtime.Start(3).has_value(), "start cancellable generation");
        std::promise<void> entered;
        auto ready = entered.get_future();
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> weak = token;
        std::atomic<bool> canceled{};
        std::jthread waiter{ [&runtime, &entered, &canceled, token = std::move(token)]() mutable {
            try { mhttp::SyncWait(runtime, cancellable_sleep(entered, std::move(token))); }
            catch (std::system_error const& error) { canceled = error.code() == mhttp::Error::Canceled; }
        } };
        if (!Check(ready.wait_for(1s) == std::future_status::ready, "Task begins before stop")) { std::abort(); }
        if (i % 2 == 0) { std::this_thread::sleep_for(1ms); }
        auto const stopping = std::chrono::steady_clock::now();
        runtime.Stop();
        Check(runtime.Join().has_value(), "Stop drains canceled timer completion");
        waiter.join();
        Check(canceled, "pending Sleep reports mhttp Canceled");
        Check(weak.expired(), "canceled coroutine parameters are released");
        Check(std::chrono::steady_clock::now() - stopping < 1s, "ten-second timer does not delay stopping");
    }
}

/**
 * @brief Own several sibling Tasks that deliberately compete for one root scheduler.
 * @note The awaiting root owns this object until all children reach final suspension.
 */
struct StrandGroup {
    std::vector<Task<void>> children; ///< Child frames retained until the group finishes.
    std::shared_ptr<mhttp::detail::Scheduler> scheduler; ///< Shared root strand.
    std::coroutine_handle<> parent; ///< Borrowed awaiting root frame.
    std::atomic<unsigned> remaining{ 12 }; ///< Children not yet finished.
    std::atomic<unsigned> active{}; ///< Detect simultaneous user code on the same strand.

    /**
     * @brief Await asynchronous yields and check mutual exclusion between sibling continuations.
     * @param group Parent-owned group, alive through all child completions.
     */
    static auto Probe(StrandGroup& group) -> Task<void> {
        for (unsigned i{}; i < 20; ++i) {
            co_await mhttp::Sleep(0ms);
            CheckEq(group.active.fetch_add(1), 0U, "same-strand user code never overlaps");
            for (unsigned spin{}; spin < 40; ++spin) { std::this_thread::yield(); }
            CheckEq(group.active.fetch_sub(1), 1U, "same-strand section remains exclusive");
        }
        if (group.remaining.fetch_sub(1) == 1) { group.scheduler->Schedule(group.parent); }
    }

    /**
     * @brief Build unstarted child frames owned by this awaiter.
     */
    StrandGroup() {
        for (unsigned i{}; i < remaining.load(); ++i) { children.push_back(Probe(*this)); }
    }

    /**
     * @brief Suspend while all owned children execute.
     * @return False.
     */
    auto await_ready() const noexcept -> bool { return false; }

    /**
     * @brief Launch children on the awaiting root's inherited scheduler.
     * @tparam Promise Awaiting Task promise type.
     * @param frame Borrowed root frame, retained by SyncWait's asynchronous owner.
     */
    template <typename Promise>
    auto await_suspend(std::coroutine_handle<Promise> frame) -> void {
        parent = frame;
        scheduler = frame.promise().scheduler;
        for (auto& child : children) {
            mhttp::detail::TaskAccess::Bind(child, scheduler);
            scheduler->Schedule(mhttp::detail::TaskAccess::Handle(child));
        }
    }

    /**
     * @brief Check that queued root resumption follows every child's final suspension.
     */
    auto await_resume() -> void {
        for (auto const& child : children) {
            Check(mhttp::detail::TaskAccess::Handle(child).done(), "group children finish before root resumes");
        }
    }
};

/**
 * @brief Keep a sibling group inside the root frame until its last completion resumes it.
 */
auto strand_root() -> Task<void> {
    StrandGroup group;
    co_await group;
}

/**
 * @brief Verify hundreds of sibling timer completions serialize on a multithreaded Runtime.
 */
auto strand_serialization() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start strand runtime with competing I/O threads");
    mhttp::SyncWait(runtime, strand_root());
    runtime.Stop();
    Check(runtime.Join().has_value(), "join strand runtime after child cleanup");
}

/**
 * @brief Run asynchronous Task/runtime cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "values and failures", values_and_failures },
        { "rejected waits", rejected_waits },
        { "timer and cleanup", timer_and_cleanup },
        { "stop pending Sleep", stop_pending_sleep },
        { "strand serialization", strand_serialization },
    });
}
