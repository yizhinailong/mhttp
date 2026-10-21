/**
 * @file task_test.cpp
 * @brief Verify Task values, lazy frame ownership, scheduler inheritance, and symmetric transfer.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::Task;
using mhttp::test::Check;
using mhttp::test::CheckEq;

static_assert(!std::copy_constructible<Task<int>>);
static_assert(!std::is_copy_assignable_v<Task<void>>);
static_assert(std::is_nothrow_move_constructible_v<Task<int>>);
static_assert(std::is_nothrow_move_assignable_v<Task<void>>);

/**
 * @brief Execute an entirely synchronous Task tree while retaining the root until final suspension.
 * @tparam T Root result type.
 * @param task Owned root; must not suspend for external I/O.
 * @return Root result, if non-void.
 */
template <typename T>
auto run_now(Task<T> task) -> T {
    auto const frame = mhttp::detail::TaskAccess::Handle(task);
    frame.resume();
    if (!Check(frame.done(), "synchronous Task tree finishes in one root resume")) {
        throw std::logic_error{ "unexpected external suspension" };
    }
    if (frame.promise().exception) { std::rethrow_exception(frame.promise().exception); }
    if constexpr (!std::is_void_v<T>) { return std::move(*frame.promise().value); }
}

/**
 * @brief Return a move-only value with no default-constructed pointee.
 * @return Owned result.
 */
auto make_value() -> Task<std::unique_ptr<int>> { co_return std::make_unique<int>(42); }

/**
 * @brief Transfer a move-only child result into a scalar result.
 * @return Observed pointee.
 */
auto nested_value() -> Task<int> {
    auto child = make_value();
    auto result = co_await std::move(child);
    Check(!child, "co_await consumes sole Task ownership");
    co_return *result;
}

/**
 * @brief Complete a void child and expose that its body ran.
 * @param calls Borrowed counter, alive until the Task completes.
 */
auto void_child(int& calls) -> Task<void> { ++calls; co_return; }

/**
 * @brief Await a void child from a value-returning parent.
 * @return Number of child body executions.
 */
auto nested_void() -> Task<int> {
    int calls{};
    co_await void_child(calls);
    co_return calls;
}

/**
 * @brief Throw from a leaf coroutine body.
 * @return Never returns normally.
 */
auto failing_leaf() -> Task<int> { throw std::runtime_error{ "leaf failure" }; co_return 0; }

/**
 * @brief Rethrow a leaf exception through an intermediate coroutine.
 * @return Never returns normally.
 */
auto failing_parent() -> Task<int> { co_return co_await failing_leaf(); }

/**
 * @brief Catch a propagated child exception inside its grandparent coroutine.
 * @return Whether the original exception type and message survived.
 */
auto catch_failure() -> Task<bool> {
    try { static_cast<void>(co_await failing_parent()); }
    catch (std::runtime_error const& error) { co_return std::string_view{ error.what() } == "leaf failure"; }
    co_return false;
}

/**
 * @brief Verify normal values, void, move-only results, and caught/uncaught exceptions.
 */
auto values_and_exceptions() -> void {
    CheckEq(run_now(nested_value()), 42, "move-only value survives nested await");
    CheckEq(run_now(nested_void()), 1, "void child runs exactly once");
    Check(run_now(catch_failure()), "exception reaches the awaiting coroutine unchanged");
    bool caught{};
    try { static_cast<void>(run_now(failing_parent())); }
    catch (std::runtime_error const& error) { caught = std::string_view{ error.what() } == "leaf failure"; }
    Check(caught, "uncaught parent exception reaches root owner");
}

/**
 * @brief Count frame-local objects across nested execution and destruction.
 */
struct LiveGuard {
    int& count; ///< Borrowed live-object counter.

    /**
     * @brief Increment the referenced counter.
     * @param counter Alive until this guard is destroyed.
     */
    explicit LiveGuard(int& counter) : count{ counter } { ++count; }

    /**
     * @brief Decrement on scope exit or coroutine-frame destruction.
     */
    ~LiveGuard() { --count; }
};

/**
 * @brief Build a deep lazy chain without calling resume recursively.
 * @param depth Remaining child depth.
 * @param live Counter alive until root destruction.
 * @return Total depth reached.
 */
auto deep_chain(int depth, int& live) -> Task<int> {
    LiveGuard guard{ live };
    if (depth == 0) { co_return 0; }
    co_return 1 + co_await deep_chain(depth - 1, live);
}

/**
 * @brief Verify ten thousand nested awaits complete and destroy all frame-local objects.
 */
auto symmetric_transfer() -> void {
    int live{};
    CheckEq(run_now(deep_chain(10'000, live)), 10'000, "deep await chain does not overflow the call stack");
    CheckEq(live, 0, "deep chain releases every local object");
}

/**
 * @brief Keep a parameter alive even though the body is lazy.
 * @param token Owned lifetime token.
 * @param calls Borrowed body execution counter.
 */
auto lazy_owner(std::shared_ptr<int> token, int& calls) -> Task<void> {
    ++calls;
    CheckEq(*token, 1, "owned lazy parameter remains alive");
    co_return;
}

/**
 * @brief Verify unstarted frame destruction, moved-from owners, and move assignment cleanup.
 */
auto lazy_ownership() -> void {
    int calls{};
    std::weak_ptr<int> first;
    std::weak_ptr<int> second;
    {
        auto token = std::make_shared<int>(1);
        first = token;
        auto original = lazy_owner(std::move(token), calls);
        CheckEq(calls, 0, "creation never enters the body");
        auto moved = std::move(original);
        Check(!original && moved, "move transfers frame without running it");
        auto replacement = std::make_shared<int>(1);
        second = replacement;
        original = lazy_owner(std::move(replacement), calls);
        moved = std::move(original);
        Check(first.expired(), "move assignment destroys overwritten lazy frame");
        Check(!second.expired(), "replacement frame owns its parameter");
    }
    Check(first.expired() && second.expired(), "unawaited frames release all owned parameters");
    CheckEq(calls, 0, "destroying unawaited frames never runs body code");
    auto token = std::make_shared<int>(1);
    auto runnable = lazy_owner(token, calls);
    auto* alias = &runnable;
    runnable = std::move(*alias);
    run_now(std::move(runnable));
    CheckEq(calls, 1, "self move leaves sole ownership intact");
}

/**
 * @brief Expose a deliberately suspended leaf handle to this test's synchronous owner.
 */
struct PauseAwaiter {
    std::coroutine_handle<>& paused; ///< Borrowed destination, alive until the test resumes or destroys it.

    /**
     * @brief Always suspend the leaf.
     * @return False.
     */
    auto await_ready() const noexcept -> bool { return false; }

    /**
     * @brief Publish the suspended leaf without scheduling external work.
     * @param frame Borrowed leaf owned by the Task tree.
     */
    auto await_suspend(std::coroutine_handle<> frame) const noexcept -> void { paused = frame; }

    /**
     * @brief Continue after the synchronous test owner resumes the leaf.
     */
    auto await_resume() const noexcept -> void {}
};

/**
 * @brief Suspend a nested Task tree with observable live locals.
 * @param depth Remaining child depth.
 * @param live Borrowed live-object count.
 * @param paused Borrowed leaf handle destination.
 */
auto paused_chain(int depth, int& live, std::coroutine_handle<>& paused) -> Task<void> {
    LiveGuard guard{ live };
    if (depth == 0) { co_await PauseAwaiter{ paused }; }
    else { co_await paused_chain(depth - 1, live, paused); }
}

/**
 * @brief Verify child frames survive suspension and cleanup on completion or abandoned local testing.
 */
auto suspended_ownership() -> void {
    int live{};
    std::coroutine_handle<> paused;
    {
        auto root = paused_chain(3, live, paused);
        auto const frame = mhttp::detail::TaskAccess::Handle(root);
        frame.resume();
        Check(!frame.done() && paused && live == 4, "nested suspension retains parent and child locals");
        paused.resume();
        paused = {};
        Check(frame.done() && live == 0, "leaf completion resumes parents and releases locals");
    }
    {
        auto root = paused_chain(3, live, paused);
        mhttp::detail::TaskAccess::Handle(root).resume();
        paused = {};
        // No external completion owns a resumption, so this test may destroy the suspended root.
    }
    CheckEq(live, 0, "destroying a locally suspended tree releases all child frames");
}

/**
 * @brief Queue deterministic resumptions without an I/O executor.
 */
class ManualScheduler : public mhttp::detail::Scheduler {
public:
    std::deque<std::coroutine_handle<>> ready; ///< Borrowed handles retained by their root tree.

    /**
     * @brief Append a handle without resuming it inline.
     * @param continuation Suspended frame owned by the test until draining finishes.
     */
    auto Schedule(std::coroutine_handle<> continuation) -> void override { ready.push_back(continuation); }
};

/**
 * @brief Observe the inherited scheduler and suspend by scheduling this coroutine once.
 */
struct RequeueAwaiter {
    std::shared_ptr<mhttp::detail::Scheduler>& observed; ///< Borrowed scheduler observation.

    /**
     * @brief Always enqueue a resumption.
     * @return False.
     */
    auto await_ready() const noexcept -> bool { return false; }

    /**
     * @brief Use the Task promise scheduler without knowing its concrete backend.
     * @tparam Promise Awaiting Task promise.
     * @param frame Borrowed frame owned by the root Task.
     */
    template <typename Promise>
    auto await_suspend(std::coroutine_handle<Promise> frame) const -> void {
        observed = frame.promise().scheduler;
        if (!observed) { throw std::logic_error{ "missing scheduler" }; }
        observed->Schedule(frame);
    }

    /**
     * @brief Complete after the queued resumption.
     */
    auto await_resume() const noexcept -> void {}
};

/**
 * @brief Verify child scheduler propagation across several nested Tasks.
 * @param depth Remaining child depth.
 * @param observed Borrowed observation, alive until completion.
 */
auto scheduled_child(int depth, std::shared_ptr<mhttp::detail::Scheduler>& observed) -> Task<void> {
    if (depth == 0) { co_await RequeueAwaiter{ observed }; }
    else { co_await scheduled_child(depth - 1, observed); }
}

/**
 * @brief Verify the inherited scheduler enqueues without executing inline.
 */
auto scheduler_inheritance() -> void {
    auto scheduler = std::make_shared<ManualScheduler>();
    std::shared_ptr<mhttp::detail::Scheduler> observed;
    auto task = scheduled_child(3, observed);
    mhttp::detail::TaskAccess::Bind(task, scheduler);
    auto const frame = mhttp::detail::TaskAccess::Handle(task);
    frame.resume();
    Check(!frame.done() && observed == scheduler, "grandchild inherits root scheduler");
    CheckEq(scheduler->ready.size(), std::size_t{ 1 }, "external completion enqueues exactly once");
    while (!scheduler->ready.empty()) {
        auto const continuation = scheduler->ready.front();
        scheduler->ready.pop_front();
        continuation.resume();
    }
    Check(frame.done() && !frame.promise().exception, "queued resumption finishes the complete tree");
}

/**
 * @brief Diagnose an empty/moved Task through an ordinary coroutine exception.
 * @return Whether misuse was reported without dereferencing an empty handle.
 */
auto await_empty() -> Task<bool> {
    try { co_await Task<void>{}; }
    catch (std::logic_error const&) { co_return true; }
    co_return false;
}

/**
 * @brief Verify empty Task misuse is recoverable.
 */
auto empty_task() -> void { Check(run_now(await_empty()), "empty Task await throws logic_error"); }

/**
 * @brief Run Task cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "values and exceptions", values_and_exceptions },
        { "symmetric transfer", symmetric_transfer },
        { "lazy ownership", lazy_ownership },
        { "suspended ownership", suspended_ownership },
        { "scheduler inheritance", scheduler_inheritance },
        { "empty task", empty_task },
    });
}
