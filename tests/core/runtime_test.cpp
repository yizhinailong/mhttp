/**
 * @file runtime_test.cpp
 * @brief Verify runtime lifecycle, restart, concurrent stop/join, and inline execution.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Verify idempotent stop/join and explicit Start errors across repeated generations.
 */
auto threaded_lifecycle() -> void {
    mhttp::Runtime runtime;
    Check(!runtime.IsRunning() && !mhttp::Runtime::IsIoThread(), "new runtime and calling thread are idle");
    runtime.Stop();
    Check(runtime.Join().has_value(), "joining idle runtime succeeds");
    auto const zero = runtime.Start(0);
    Check(!zero && zero.error() == mhttp::Error::InvalidRequest, "zero execution threads rejected");
    for (std::size_t i{}; i < 50; ++i) {
        Check(runtime.Start(1 + i % 4).has_value(), "start fresh generation");
        Check(runtime.IsRunning(), "Start publishes running state before returning");
        auto const duplicate = runtime.Start();
        Check(!duplicate && duplicate.error() == mhttp::Error::AlreadyRunning, "duplicate Start reports AlreadyRunning");
        auto const duplicate_run = runtime.Run();
        Check(!duplicate_run && duplicate_run.error() == mhttp::Error::AlreadyRunning, "Run cannot nest a started generation");
        runtime.Stop();
        runtime.Stop();
        Check(!runtime.IsRunning(), "Stop immediately rejects new work");
        Check(!runtime.Start(), "restart requires draining and joining the old generation");
        Check(runtime.Join().has_value(), "join all execution threads");
        Check(runtime.Join().has_value(), "repeat Join safely");
    }
}

/**
 * @brief Verify multiple external callers can stop and join the same generation safely.
 */
auto concurrent_stop_join() -> void {
    mhttp::Runtime runtime;
    for (unsigned i{}; i < 20; ++i) {
        Check(runtime.Start(4).has_value(), "start concurrent-stop generation");
        std::vector<std::jthread> callers;
        for (unsigned caller{}; caller < 4; ++caller) {
            callers.emplace_back([&runtime] {
                runtime.Stop();
                Check(runtime.Join().has_value(), "concurrent stop/join finishes");
            });
        }
        callers.clear();
        Check(!runtime.IsRunning(), "all joiners observe a stopped runtime");
    }
}

/**
 * @brief Verify Run blocks on its calling thread and external Stop/Join drains it.
 */
auto inline_run() -> void {
    mhttp::Runtime runtime;
    std::atomic<bool> finished{};
    std::jthread runner{ [&runtime, &finished] {
        Check(runtime.Run().has_value(), "inline Run exits after external Stop");
        finished = true;
    } };
    auto const deadline = std::chrono::steady_clock::now() + 1s;
    while (!runtime.IsRunning() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    if (!Check(runtime.IsRunning(), "inline Run publishes active generation")) { std::abort(); }
    Check(!finished, "inline Run stays alive while its work guard is active");
    runtime.Stop();
    Check(runtime.Join().has_value(), "Join waits for inline runner to leave the context");
    runner.join();
    Check(finished && !runtime.IsRunning(), "inline runner drained");
    Check(runtime.Start().has_value(), "threaded mode can follow a joined inline generation");
    runtime.Stop();
    Check(runtime.Join().has_value(), "joined restarted generation");
}

/**
 * @brief Verify destructor joins a still-running idle generation without an explicit Stop.
 */
auto destructor_cleanup() -> void {
    auto const before = std::chrono::steady_clock::now();
    for (unsigned i{}; i < 20; ++i) {
        mhttp::Runtime runtime;
        Check(runtime.Start(2).has_value(), "start destructed generation");
    }
    Check(std::chrono::steady_clock::now() - before < 2s, "destructor stops and joins promptly");
}

/**
 * @brief Run runtime lifecycle cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "threaded lifecycle", threaded_lifecycle },
        { "concurrent stop and join", concurrent_stop_join },
        { "inline Run", inline_run },
        { "destructor cleanup", destructor_cleanup },
    });
}
