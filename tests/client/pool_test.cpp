/**
 * @file pool_test.cpp
 * @brief Verify bounded pool admission, fairness, ownership, idle reclamation, and terminal cancellation.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"
#include "../support/thread_task.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;
using mhttp::client::ClientPool;

/**
 * @brief Observe a metadata transition within a bounded test-only blocking wait.
 * @param predicate Thread-safe observation.
 * @return Whether the transition occurred within one second.
 */
auto eventually(auto predicate) -> bool {
    auto const until = std::chrono::steady_clock::now() + 1s;
    while (!predicate() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(1ms); }
    return predicate();
}

/**
 * @brief Reject invalid capacity, timer, and underlying client configuration without starting work.
 */
auto validation() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start validation runtime");
    for (unsigned index{}; index < 7; ++index) {
        mhttp::client::ClientPoolOptions options;
        if (index == 0) { options.max_per_origin = 0; }
        if (index == 1) { options.max_total = 0; }
        if (index == 2) { options.max_waiters = 0; }
        if (index == 3) { options.idle_timeout = -1ms; }
        if (index == 4) { options.idle_check_interval = 0ms; }
        if (index == 5) { options.idle_check_interval = std::chrono::milliseconds::max(); }
        if (index == 6) { options.client_options.response_chunk_bytes = 0; }
        ClientPool pool{ runtime, options };
        Check(pool.InitializationError() == mhttp::Error::InvalidRequest, "invalid pool has an inspectable error");
        auto result = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80));
        Check(!result && result.error() == mhttp::Error::InvalidRequest, "invalid pool acquisition does no I/O");
        CheckEq(pool.Stats().Total(), std::size_t{}, "invalid pool allocates no clients");
    }
}

/**
 * @brief Preserve move-only handle semantics, idempotent release, bounded reuse, and mutable client defaults.
 */
auto handles_and_reuse() -> void {
    static_assert(!std::copy_constructible<mhttp::client::ClientHandle>);
    static_assert(std::is_nothrow_move_constructible_v<mhttp::client::ClientHandle>);
    mhttp::client::ClientHandle empty;
    Check(!empty && empty.Get() == nullptr, "default handle is empty");
    empty.Release();
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start handle runtime");
    ClientPool pool{ runtime, { .max_per_origin = 1, .max_total = 2 } };
    mhttp::client::HttpClient* original{};
    for (unsigned index{}; index < 30; ++index) {
        auto result = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80));
        if (!Check(result.has_value(), "borrow reusable lazy client")) { return; }
        auto handle = std::move(*result);
        Check(!*result && handle && handle.Get() == &*handle && !handle->IsOpen(), "move leaves source empty without connecting");
        if (!original) { original = handle.Get(); }
        CheckEq(handle.Get(), original, "same origin returns the same client wrapper");
        CheckEq(pool.Stats().active, std::size_t{ 1 }, "one active loan");
        empty = std::move(handle);
        Check(!handle && empty, "move assignment transfers loan");
        empty.Release(); empty.Release();
        CheckEq(pool.Stats().idle, std::size_t{ 1 }, "idempotent release returns one slot");
    }
    auto missing = pool.Stats("missing", 80);
    Check(missing && missing->Total() == 0 && missing->waiting == 0, "unknown origin statistics do not allocate a route");
}

/**
 * @brief Normalize URL paths, default ports, host case, and numeric IPv6 spelling consistently for borrowing and stats.
 */
auto origin_identity() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start origin runtime");
    ClientPool pool{ runtime, { .max_per_origin = 1 } };
    std::string url{ "HTTP://Example.COM:80/a?q=1#fragment" };
    auto task = pool.Acquire(url);
    url.clear();
    auto result = mhttp::SyncWait(runtime, std::move(task));
    if (!Check(result.has_value(), "owned URL survives caller mutation")) { return; }
    CheckEq((*result)->Host(), std::string_view{ "example.com" }, "host normalized before borrowing");
    auto* pointer = result->Get();
    auto stat = pool.Stats("http://example.com/other?q=2");
    Check(stat && stat->active == 1, "statistics and acquire ignore URL path/query identically");
    result->Release();
    auto again = mhttp::SyncWait(runtime, pool.Acquire("example.com", 80));
    Check(again && again->Get() == pointer, "explicit and default ports have one identity");
    again->Release();
    auto ipv6 = mhttp::SyncWait(runtime, pool.Acquire("[0:0:0:0:0:0:0:1]", 8080));
    if (Check(ipv6.has_value(), "borrow numeric IPv6 lazily")) {
        auto normalized = pool.Stats("http://[::1]:8080/a");
        Check(normalized && normalized->active == 1, "equivalent IPv6 literals share capacity and stats");
    }
    for (auto const& [input, error] : std::to_array<std::pair<std::string_view, mhttp::Error>>({
        { "", mhttp::Error::InvalidUrl }, { "http://user@host/", mhttp::Error::InvalidUrl },
        { "https://example.com/", mhttp::Error::UnsupportedScheme } })) {
        auto invalid = mhttp::SyncWait(runtime, pool.Acquire(input));
        Check(!invalid && invalid.error() == error, "invalid origin rejected without network activity");
        CheckEq(pool.Stats(input).error(), error, "statistics validate origins consistently");
    }
}

/**
 * @brief Enforce per-origin capacity, acquisition deadlines, nonpositive fail-fast, and a bounded waiter queue.
 */
auto capacity_and_waiters() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start capacity runtime");
    ClientPool pool{ runtime, { .max_per_origin = 1, .max_total = 1, .max_waiters = 1 } };
    auto held = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80));
    if (!Check(held.has_value(), "hold sole pool slot")) { return; }
    for (auto wait : { 0ms, -1ms, 35ms }) {
        auto const started = std::chrono::steady_clock::now();
        auto result = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80, wait));
        Check(!result && result.error() == mhttp::Error::AcquireTimeout, "bounded or fail-fast capacity timeout");
        Check(std::chrono::steady_clock::now() - started < 500ms, "timeout does not block an I/O worker");
        CheckEq(pool.Stats().waiting, std::size_t{}, "timed-out waiter is fully removed");
    }
    auto excessive = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80, std::chrono::steady_clock::duration::max()));
    Check(!excessive && excessive.error() == mhttp::Error::InvalidRequest, "reject overflowing wait duration");
    mhttp::test::ThreadTask queued{ [&] { return mhttp::SyncWait(runtime, pool.Acquire("localhost", 80, 2s)); } };
    Check(eventually([&] { return pool.Stats().waiting == 1; }), "first capacity waiter enters queue");
    auto overflow = mhttp::SyncWait(runtime, pool.Acquire("other", 80, 1s));
    Check(!overflow && overflow.error() == mhttp::Error::Busy, "full queue rejects excess demand");
    auto filter = pool.Stats("localhost", 80);
    Check(filter && filter->active == 1 && filter->waiting == 1, "origin stats include queued demand");
    held->Release();
    auto granted = queued.Get();
    Check(granted && pool.Stats().active == 1 && pool.Stats().waiting == 0, "return wakes and reserves capacity for queued waiter");
}

/**
 * @brief Grant the oldest eligible origin while permitting unrelated origins to proceed.
 */
auto fair_admission() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start fairness runtime");
    ClientPool pool{ runtime, { .max_per_origin = 1, .max_total = 3 } };
    auto held = mhttp::SyncWait(runtime, pool.Acquire("first", 80));
    mhttp::test::ThreadTask first{ [&] { return mhttp::SyncWait(runtime, pool.Acquire("first", 80, 2s)); } };
    Check(eventually([&] { return pool.Stats().waiting == 1; }), "first same-origin waiter queued");
    mhttp::test::ThreadTask second{ [&] { return mhttp::SyncWait(runtime, pool.Acquire("first", 80, 2s)); } };
    Check(eventually([&] { return pool.Stats().waiting == 2; }), "second same-origin waiter queued");
    auto independent = mhttp::SyncWait(runtime, pool.Acquire("other", 80, 0ms));
    Check(independent && pool.Stats().active == 2, "blocked first origin does not block independent capacity");
    held->Release();
    auto oldest = first.Get();
    Check(oldest && second.WaitFor(0ms) == std::future_status::timeout, "oldest matching waiter owns the next return");
    oldest->Release();
    auto next = second.Get();
    Check(next && pool.Stats().waiting == 0, "next return serves second waiter");
}

/**
 * @brief Reclaim idle global capacity on independent timer ticks and wake a different origin's demand.
 */
auto idle_eviction() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start eviction runtime");
    ClientPool pool{ runtime, { .max_total = 1, .idle_timeout = 40ms, .idle_check_interval = 200ms } };
    auto held = mhttp::SyncWait(runtime, pool.Acquire("old", 80));
    held->Release();
    std::this_thread::sleep_for(80ms);
    CheckEq(pool.Stats().idle, std::size_t{ 1 }, "expiration age alone does not change the independent check interval");
    auto other = mhttp::SyncWait(runtime, pool.Acquire("new", 80, 1s));
    Check(other && pool.Stats().active == 1 && pool.Stats().idle == 0, "timer eviction wakes an otherwise globally blocked origin");
    CheckEq(pool.Stats("old", 80)->Total(), std::size_t{}, "expired route removed");
    other->Release();
    Check(eventually([&] { return pool.Stats().Total() == 0; }), "periodic eviction retires returned slot");
    ClientPool disabled{ runtime, { .idle_timeout = 0ms, .idle_check_interval = 5ms } };
    auto lazy = mhttp::SyncWait(runtime, disabled.Acquire("old", 80));
    lazy->Release();
    std::this_thread::sleep_for(30ms);
    CheckEq(disabled.Stats().idle, std::size_t{ 1 }, "zero idle timeout disables timed eviction");
}

/**
 * @brief Keep slots active through unstarted Tasks, returning them when their final owning frame is destroyed.
 */
auto issued_task_ownership() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start lazy ownership runtime");
    ClientPool pool{ runtime, { .max_per_origin = 1 } };
    auto held = mhttp::SyncWait(runtime, pool.Acquire("localhost", 0));
    auto request = (*held)->Get();
    auto probe = (*held)->IsAlive();
    held->Release();
    CheckEq(pool.Stats().active, std::size_t{ 1 }, "issued operations retain a released handle's loan");
    request = {};
    CheckEq(pool.Stats().active, std::size_t{ 1 }, "another issued probe still retains the loan");
    probe = {};
    CheckEq(pool.Stats().idle, std::size_t{ 1 }, "destroying final unstarted Task returns the slot");
    auto acquired = mhttp::SyncWait(runtime, pool.Acquire("localhost", 0));
    auto pending = (*acquired)->Get();
    acquired->Release();
    pool.Stop();
    auto canceled = mhttp::SyncWait(runtime, std::move(pending));
    Check(!canceled && canceled.error() == mhttp::Error::Canceled, "pool Stop cancels an issued lazy transfer");
    CheckEq(pool.Stats().Total(), std::size_t{}, "completed canceled Task releases final slot after Stop");
}

/**
 * @brief Stop queued demand, retained handles, post-stop requests, and lazily issued acquisition after wrapper destruction.
 */
auto terminal_shutdown() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start stop runtime");
    ClientPool pool{ runtime, { .max_per_origin = 1 } };
    auto held = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80));
    mhttp::test::ThreadTask waiting{ [&] { return mhttp::SyncWait(runtime, pool.Acquire("localhost", 80, 2s)); } };
    Check(eventually([&] { return pool.Stats().waiting == 1; }), "shutdown waiter queued");
    pool.Stop(); pool.Stop();
    auto canceled = waiting.Get();
    Check(!canceled && canceled.error() == mhttp::Error::Canceled && pool.Stopped(), "terminal Stop wakes queued demand");
    auto future = mhttp::SyncWait(runtime, pool.Acquire("other", 80));
    Check(!future && future.error() == mhttp::Error::Canceled, "stopped pool rejects new origins");
    auto send = mhttp::SyncWait(runtime, (*held)->Get());
    Check(!send && send.error() == mhttp::Error::Canceled, "retained handle cannot reconnect after pool Stop");
    held->Release();
    CheckEq(pool.Stats().Total(), std::size_t{}, "stopped pool retires last handle");
    mhttp::Task<mhttp::Result<mhttp::client::ClientHandle>> issued;
    {
        ClientPool temporary{ runtime };
        issued = temporary.Acquire(std::string{ "http://localhost/a" });
    }
    auto retired = mhttp::SyncWait(runtime, std::move(issued));
    Check(!retired && retired.error() == mhttp::Error::Canceled, "lazy acquisition owns state across wrapper destruction");
}

/**
 * @brief Transfer public pool ownership without invalidating loans, stopping any overwritten pool.
 */
auto movable_pool() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start move runtime");
    ClientPool original{ runtime };
    auto held = mhttp::SyncWait(runtime, original.Acquire("localhost", 80));
    ClientPool moved{ std::move(original) };
    Check(original.Stopped() && moved.Stats().active == 1, "move constructor transfers existing ledger");
    ClientPool destination{ runtime };
    auto replaced = mhttp::SyncWait(runtime, destination.Acquire("other", 80));
    destination = std::move(moved);
    Check(moved.Stopped() && destination.Stats().active == 1, "move assignment transfers live pool");
    auto canceled = mhttp::SyncWait(runtime, (*replaced)->Get());
    Check(!canceled && canceled.error() == mhttp::Error::Canceled, "move assignment stops overwritten borrowers");
    held->Release();
    CheckEq(destination.Stats().idle, std::size_t{ 1 }, "pre-move handle returns to transferred pool");
}

/**
 * @brief Serialize many concurrent borrowers and preserve exact slot accounting across foreign-thread releases.
 */
auto concurrent_borrowers() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start concurrent runtime");
    ClientPool pool{ runtime, { .max_per_origin = 4, .max_total = 4 } };
    std::vector<mhttp::test::ThreadTask<void>> workers;
    for (unsigned index{}; index < 8; ++index) {
        workers.emplace_back([&] {
            for (unsigned iteration{}; iteration < 50; ++iteration) {
                auto handle = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80, 2s));
                Check(handle.has_value(), "concurrent borrower obtains capacity");
                Check(pool.Stats().Total() <= 4, "global capacity never exceeded");
            }
        });
    }
    for (auto& worker : workers) { worker.Get(); }
    auto stats = pool.Stats();
    Check(stats.active == 0 && stats.waiting == 0 && stats.idle > 0 && stats.idle <= 4, "all concurrent loans return exactly once");
}

/**
 * @brief Cancel waiters on runtime stop and reject reactivation after a later generation starts.
 */
auto runtime_shutdown() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start generation runtime");
    ClientPool pool{ runtime, { .max_per_origin = 1 } };
    auto held = mhttp::SyncWait(runtime, pool.Acquire("localhost", 80));
    mhttp::test::ThreadTask waiting{ [&] { return mhttp::SyncWait(runtime, pool.Acquire("localhost", 80, 2s)); } };
    Check(eventually([&] { return pool.Stats().waiting == 1; }), "generation waiter queued");
    runtime.Stop();
    auto canceled = waiting.Get();
    Check(!canceled && canceled.error() == mhttp::Error::Canceled, "runtime stop drains capacity waiter");
    Check(runtime.Join().has_value() && pool.Stopped(), "runtime shutdown terminally stops attached pool");
    Check(runtime.Start(4).has_value(), "restart runtime generation");
    auto next = mhttp::SyncWait(runtime, pool.Acquire("other", 80));
    Check(!next && next.error() == mhttp::Error::Canceled, "runtime restart does not revive old pool");
}

/**
 * @brief Run public pool cases with bounded watchdogs.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "configuration validation", validation }, { "handles and repeated reuse", handles_and_reuse },
        { "normalized origin identity", origin_identity }, { "bounded capacity and waiters", capacity_and_waiters },
        { "fair eligible admission", fair_admission }, { "independent idle eviction", idle_eviction },
        { "issued Task loan ownership", issued_task_ownership }, { "terminal pool shutdown", terminal_shutdown },
        { "movable pool ownership", movable_pool }, { "concurrent borrowers", concurrent_borrowers },
        { "runtime generation shutdown", runtime_shutdown },
    });
}
