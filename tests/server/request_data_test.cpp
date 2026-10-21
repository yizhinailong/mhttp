/**
 * @file request_data_test.cpp
 * @brief Verify typed request context, immutable snapshots, concurrent erasure, and user-code reentry.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Keep exact type and tag identity, normalize cv/ref, and copy both tags and values.
 */
auto typed_slots() -> void {
    mhttp::server::RequestData data;
    Check(!data.Fetch<int>() && !data.Has<int>(), "missing slot returns nullopt");
    Check(!data.Erase<int>(), "missing erase is harmless");
    data.Store(42);
    data.Store("tag", 7);
    data.Store("Tag", 8);
    data.Store("tag", std::string{ "typed" });
    CheckEq(data.Fetch<int>(), std::optional{ 42 }, "untagged lookup");
    CheckEq(data.Fetch<int>("tag"), std::optional{ 7 }, "tagged integer");
    CheckEq(data.Fetch<int>("Tag"), std::optional{ 8 }, "tags are case-sensitive");
    CheckEq(data.Fetch<std::string>("tag"), std::optional<std::string>{ "typed" }, "same tag with different type");
    Check(!data.Fetch<double>("tag"), "type mismatch is absent");
    CheckEq(data.Fetch<int const&>(), std::optional{ 42 }, "cv/ref normalized lookup");
    CheckEq(data.Fetch<int volatile>(), std::optional{ 42 }, "volatile normalized lookup");
    static_assert(std::same_as<decltype(data.Fetch<int&>()), std::optional<int>>);
    Check(data.Has<int const>() && data.Erase<int const&>() && !data.Has<int>(), "presence and erasure use the same type key");
    int source{ 19 };
    data.Store(source);
    source = 20;
    CheckEq(data.Fetch<int>(), std::optional{ 19 }, "lvalue is copied into storage");
    std::string tag{ "owned" };
    data.Store(tag, std::string{ "value" });
    tag = "changed";
    CheckEq(data.Fetch<std::string>("owned"), std::optional<std::string>{ "value" }, "tag owns source bytes");
    data.Store("a\0b"sv, 9);
    CheckEq(data.Fetch<int>("a\0b"sv), std::optional{ 9 }, "binary tag bytes retained");
    Check(!data.Fetch<int>("a"), "NUL does not truncate a tag");
}

/**
 * @brief Fetch copies repeatedly and preserve fetched resource ownership after replacement and erasure.
 */
auto snapshot_ownership() -> void {
    mhttp::server::RequestData data;
    auto value = std::make_shared<int>(7);
    std::weak_ptr<int> weak{ value };
    data.Store(value);
    value.reset();
    auto first = data.Fetch<std::shared_ptr<int>>();
    auto second = std::as_const(data).Fetch<std::shared_ptr<int>>();
    Check(first && second && *first == *second && **first == 7, "Fetch copies without consuming");
    data.Store(std::make_shared<int>(8));
    Check(!weak.expired() && **first == 7, "replacement preserves prior fetched owners");
    Check(data.Erase<std::shared_ptr<int>>(), "erase current resource slot");
    Check(!data.Fetch<std::shared_ptr<int>>(), "erased storage is absent");
    first.reset();
    second.reset();
    Check(weak.expired(), "last copied owner releases resource");
    data.Store(std::string{ "one" });
    auto copy = data.Fetch<std::string>();
    copy->assign("changed");
    CheckEq(data.Fetch<std::string>(), std::optional<std::string>{ "one" }, "value copies are independent");
}

/**
 * @brief Run user code during copy and destruction to verify neither occurs under a context lock.
 */
struct ReentrantValue {
    std::function<void()> on_copy; ///< Optional copy callback.
    std::function<void()> on_destroy; ///< Optional destruction callback.

    /**
     * @brief Own callbacks for context reentry.
     * @param copied Callback invoked by copying.
     * @param destroyed Callback invoked by destruction.
     */
    ReentrantValue(std::function<void()> copied, std::function<void()> destroyed) :
        on_copy{ std::move(copied) }, on_destroy{ std::move(destroyed) } {}

    /**
     * @brief Copy callback ownership and invoke user code.
     * @param other Borrowed source.
     */
    ReentrantValue(ReentrantValue const& other) : on_copy{ other.on_copy }, on_destroy{ other.on_destroy } {
        if (on_copy) { on_copy(); }
    }

    /**
     * @brief Move callbacks without invoking copy code.
     * @param other Source owner.
     */
    ReentrantValue(ReentrantValue&& other) noexcept = default;

    /**
     * @brief Invoke destruction code if present.
     */
    ~ReentrantValue() { if (on_destroy) { on_destroy(); } }
};

/**
 * @brief Allow reentry from Fetch copies and values retired by replacement or erasure.
 */
auto user_code_reentry() -> void {
    mhttp::server::RequestData data;
    data.Store(42);
    unsigned copies{};
    unsigned destructions{};
    auto on_copy = [&] {
        CheckEq(data.Fetch<int>(), std::optional{ 42 }, "copy constructor reenters context");
        ++copies;
    };
    auto on_destroy = [&] {
        Check(data.Has<int>(), "destructor reenters context");
        ++destructions;
    };
    data.Store(ReentrantValue{ on_copy, on_destroy });
    auto fetched = data.Fetch<ReentrantValue>();
    Check(copies != 0, "Fetch invokes user copy code");
    fetched.reset();
    auto const before_replace = destructions;
    data.Store(ReentrantValue{ {}, {} });
    Check(destructions > before_replace, "replacement releases old value outside the lock");
    data.Store(ReentrantValue{ {}, on_destroy });
    auto const before_erase = destructions;
    Check(data.Erase<ReentrantValue>(), "erase reentrant value");
    Check(destructions > before_erase, "erase releases value outside the lock");
    data.Store(ReentrantValue{ [&data] { data.Erase<ReentrantValue>(); }, {} });
    auto retired = data.Fetch<ReentrantValue>();
    Check(retired && !data.Has<ReentrantValue>(), "snapshot survives erasure from its own copy constructor");
}

/**
 * @brief Ensure every concurrent Fetch returns a complete value or absence during replacement and erase.
 */
auto concurrent_context() -> void {
    mhttp::server::RequestData data;
    data.Store(5);
    std::barrier start{ 5 };
    std::vector<std::jthread> readers;
    for (unsigned reader{}; reader < 4; ++reader) {
        readers.emplace_back([&] {
            start.arrive_and_wait();
            for (unsigned index{}; index < 4000; ++index) {
                auto value = data.Fetch<int>();
                Check(!value || *value == 5, "concurrent Fetch returns one complete snapshot");
            }
        });
    }
    start.arrive_and_wait();
    for (unsigned index{}; index < 4000; ++index) {
        data.Erase<int>();
        data.Store(5);
    }
}

/**
 * @brief Run bounded request-context cases.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "typed context slots", typed_slots },
        { "context snapshot ownership", snapshot_ownership },
        { "context user code reentry", user_code_reentry },
        { "concurrent request context", concurrent_context },
    });
}
