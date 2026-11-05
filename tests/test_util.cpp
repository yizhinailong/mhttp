/**
 * @file test_util.cpp
 * @brief Verify AsyncEvent signaling semantics and when_all result ordering.
 */
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

import std;
import mhttp;

namespace {

    namespace net = boost::asio;
    using mhttp::util::AsyncEvent;

    /**
     * @brief Report a failed behavioral expectation.
     * @param condition Whether the expectation holds.
     * @param message Failure diagnostic.
     * @return Whether the expectation holds.
     */
    auto check(bool condition, std::string_view message) -> bool {
        if (!condition) {
            std::println("test_util: {}", message);
        }
        return condition;
    }

    /**
     * @brief Let other ready handlers on the executor run.
     * @param times Number of times to yield.
     */
    auto yield(int times) -> net::awaitable<void> {
        for (int i{ 0 }; i < times; ++i) {
            co_await net::post(co_await net::this_coro::executor, net::use_awaitable);
        }
    }

    /**
     * @brief Notifications without a waiter latch once and coalesce.
     * @return Whether all checks passed.
     */
    auto test_latch_and_coalesce() -> net::awaitable<bool> {
        AsyncEvent event{ co_await net::this_coro::executor };
        bool passed{ true };
        passed &= check(event.NotifyOne() == AsyncEvent::NotifyResult::Notified, "first notify must latch");
        passed &= check(event.NotifyAll() == AsyncEvent::NotifyResult::Coalesced, "repeated notify must coalesce");
        passed &= check(event.IsSignaled(), "a latched notification must be visible");
        passed &= check(co_await event.Wait() == AsyncEvent::WaitResult::Notified, "wait must consume the latched notification");
        passed &= check(!event.TryWait(), "a coalesced notification must be consumed only once");
        co_return passed;
    }

    /**
     * @brief NotifyOne wakes exactly one waiter; NotifyAll wakes the rest.
     * @return Whether all checks passed.
     */
    auto test_wakeups() -> net::awaitable<bool> {
        auto const ex{ co_await net::this_coro::executor };
        AsyncEvent event{ ex };
        std::array<std::optional<AsyncEvent::WaitResult>, 3> results;
        for (auto& result : results) {
            net::co_spawn(ex, [&event, &result]() -> net::awaitable<void> { result = co_await event.Wait(); }, net::detached);
        }
        co_await yield(3);

        bool passed{ true };
        passed &= check(event.NotifyOne() == AsyncEvent::NotifyResult::Notified, "notify with waiters must deliver");
        co_await yield(5);
        auto const woken{ std::ranges::count_if(results, [](auto const& result) { return result.has_value(); }) };
        passed &= check(woken == 1, std::format("NotifyOne must wake exactly one waiter, woke {}", woken));

        event.NotifyAll();
        co_await yield(5);
        passed &= check(std::ranges::all_of(results, [](auto const& result) { return result == AsyncEvent::WaitResult::Notified; }), "NotifyAll must wake every remaining waiter");
        passed &= check(!event.IsSignaled(), "delivered notifications must not stay latched");
        co_return passed;
    }

    /**
     * @brief Close wakes waiters with Closed, rejects notifications, and Reset reopens.
     * @return Whether all checks passed.
     */
    auto test_close_and_reset() -> net::awaitable<bool> {
        auto const ex{ co_await net::this_coro::executor };
        AsyncEvent event{ ex };
        std::optional<AsyncEvent::WaitResult> result;
        net::co_spawn(ex, [&event, &result]() -> net::awaitable<void> { result = co_await event.Wait(); }, net::detached);
        co_await yield(2);

        event.Close();
        co_await yield(5);
        bool passed{ true };
        passed &= check(result == AsyncEvent::WaitResult::Closed, "close must wake waiters with Closed");
        passed &= check(event.NotifyOne() == AsyncEvent::NotifyResult::Closed, "notify after close must report Closed");
        passed &= check(co_await event.Wait() == AsyncEvent::WaitResult::Closed, "wait after close must return Closed");

        event.Reset();
        passed &= check(!event.IsClosed(), "reset must reopen the event");
        passed &= check(event.NotifyOne() == AsyncEvent::NotifyResult::Notified, "notify after reset must latch");
        passed &= check(event.TryWait(), "try_wait after reset must consume the notification");
        co_return passed;
    }

    /**
     * @brief WaitFor gives up with TimedOut when nothing notifies.
     * @return Whether all checks passed.
     */
    auto test_wait_for_timeout() -> net::awaitable<bool> {
        AsyncEvent event{ co_await net::this_coro::executor };
        auto const start{ std::chrono::steady_clock::now() };
        auto const result{ co_await event.WaitFor(std::chrono::milliseconds{ 20 }) };
        auto const elapsed{ std::chrono::steady_clock::now() - start };
        bool passed{ true };
        passed &= check(result == AsyncEvent::WaitResult::TimedOut, "WaitFor without notification must time out");
        passed &= check(elapsed >= std::chrono::milliseconds{ 15 }, "WaitFor must not return before its timeout");
        co_return passed;
    }

    /**
     * @brief Complete after a delay with a given value.
     * @param delay Time to wait.
     * @param value Value to return.
     * @return `value`.
     */
    auto delayed(std::chrono::milliseconds delay, int value) -> net::awaitable<int> {
        net::steady_timer timer{ co_await net::this_coro::executor, delay };
        co_await timer.async_wait(net::use_awaitable);
        co_return value;
    }

    /**
     * @brief when_all returns results by operation index, not completion order.
     * @return Whether all checks passed.
     */
    auto test_when_all_order() -> net::awaitable<bool> {
        std::vector<net::awaitable<int>> ops;
        ops.push_back(delayed(std::chrono::milliseconds{ 30 }, 1));
        ops.push_back(delayed(std::chrono::milliseconds{ 1 }, 2));
        ops.push_back(delayed(std::chrono::milliseconds{ 15 }, 3));
        auto const results{ co_await mhttp::util::when_all(std::move(ops)) };
        co_return check(results == std::vector<int>{ 1, 2, 3 }, "when_all must keep operation order");
    }

    /**
     * @brief Run one coroutine test to completion on a fresh io_context.
     * @param test Test coroutine factory.
     * @return The test's result.
     */
    auto run(auto test) -> bool {
        net::io_context ioc;
        auto result{ net::co_spawn(ioc, test(), net::use_future) };
        ioc.run();
        return result.get();
    }

    /**
     * @brief Query parameters decode, convert, and encode like form-urlencoded data.
     * @return Whether all checks passed.
     */
    auto test_query_params() -> bool {
        mhttp::QueryParams params;
        params.Decode("a=1&b=x+y%21&a=2&flag=true&empty&&c=k%3Dv=w");
        bool passed{ true };
        passed &= check(params.At<int>("a") == 1, "At<int> must convert the first value");
        passed &= check(params.All<int>("a") == std::vector<int>{ 1, 2 }, "All must keep insertion order");
        passed &= check(params.At("b") == "x y!", "'+' must decode as space and %21 as '!'");
        passed &= check(params.At<bool>("flag"), "At<bool> must accept \"true\"");
        passed &= check(params.Has("empty") && params.At("empty").empty(), "a key without '=' must map to an empty value");
        passed &= check(params.At("c") == "k=v=w", "only the first '=' separates key and value");
        passed &= check(!params.Has(""), "empty parts between '&' must be skipped");

        bool threw{ false };
        try {
            static_cast<void>(params.At<int>("b"));
        } catch (std::runtime_error const&) {
            threw = true;
        }
        passed &= check(threw, "a failed numeric conversion must throw");

        mhttp::QueryParams out;
        out.Add("name", std::string_view{ "a b&c" });
        out.Add("n", 7);
        out.Add("ok", false);
        passed &= check(out.Encoded() == "n=7&name=a+b%26c&ok=false", std::format("Encoded must percent-encode, got {}", out.Encoded()));
        mhttp::QueryParams round_trip;
        round_trip.Decode(out.Encoded());
        passed &= check(round_trip.At("name") == "a b&c", "Encoded/Decode must round-trip spaces (httplib did not)");
        return passed;
    }

    /**
     * @brief Percent-encoding helpers handle reserved and malformed input.
     * @return Whether all checks passed.
     */
    auto test_url_coding() -> bool {
        bool passed{ true };
        passed &= check(mhttp::url::url_decode("a%20b%2Fc+d") == "a b/c+d", "url_decode must decode escapes and keep '+'");
        passed &= check(mhttp::url::url_decode("bad%zz%4") == "bad%zz%4", "malformed escapes must stay verbatim");
        passed &= check(mhttp::url::url_encode("a-b_c.d~e f/") == "a-b_c.d~e+f%2f", "url_encode must keep unreserved characters only");
        return passed;
    }

    /**
     * @brief Owned headers deep-copy; borrowed views alias; iteration keeps duplicates.
     * @return Whether all checks passed.
     */
    auto test_headers() -> bool {
        mhttp::Headers headers;
        headers.Set(mhttp::Field::content_type, "text/plain");
        headers.Insert("X-Dup", "1");
        headers.Insert("x-dup", "2");
        bool passed{ true };
        passed &= check(headers["CONTENT-TYPE"] == "text/plain", "name lookup must be case-insensitive");
        passed &= check(headers.Count("X-Dup") == 2 && headers.Values("X-Dup") == std::vector<std::string_view>{ "1", "2" }, "duplicates must be kept in order");
        passed &= check(headers.Size() == 3, "Size must count every line");

        mhttp::Headers copy{ headers };
        copy.Set("X-New", "n");
        passed &= check(!headers.Has("X-New"), "copying owned headers must deep-copy");

        std::size_t lines{ 0 };
        for (auto const& header : headers) {
            lines += header.value.empty() ? 0 : 1;
        }
        passed &= check(lines == 3, "iteration must visit every line");

        headers.Erase("x-dup");
        passed &= check(!headers.Has("X-Dup") && headers.Size() == 1, "Erase must drop every value of a field");
        bool threw{ false };
        try {
            static_cast<void>(headers.At("Missing"));
        } catch (std::out_of_range const&) {
            threw = true;
        }
        passed &= check(threw, "At must throw for missing fields");
        return passed;
    }

    /**
     * @brief Form data owns its files and finds fields by name.
     * @return Whether all checks passed.
     */
    auto test_form_data() -> bool {
        auto const path{ std::filesystem::temp_directory_path() / "mhttp-test-form-data.bin" };
        std::ofstream{ path } << "x";
        bool passed{ true };
        {
            mhttp::FormData form;
            form.params.remove_uploaded_files = true;
            form.fields.push_back(mhttp::FormData::Field{ .name = "a", .content = "1" });
            form.fields.push_back(mhttp::FormData::Field{ .name = "f", .filename = "f.bin", .file_path = path });
            passed &= check(form.Content("a") == "1" && form.HasContent("f") && !form.HasData("missing"), "lookups must find fields by name");
            mhttp::FormData moved{ std::move(form) };
            passed &= check(moved.FieldByName("f").has_value(), "moving must transfer the fields");
        }
        passed &= check(!std::filesystem::exists(path), "remove_uploaded_files must delete owned files on destruction");
        return passed;
    }

    /**
     * @brief mhttp::Error codes convert to error_code and describe themselves.
     * @return Whether all checks passed.
     */
    auto test_error_codes() -> bool {
        boost::system::error_code const ec{ mhttp::Error::InvalidJson };
        bool passed{ true };
        passed &= check(ec == mhttp::Error::InvalidJson && ec.category().name() == std::string_view{ "mhttp" }, "Error must convert into the mhttp category");
        passed &= check(ec.message() == "body is not valid JSON", "messages must describe the error");
        return passed;
    }

} // namespace

/**
 * @brief Run every utility test.
 * @return Zero when all checks pass, otherwise one.
 */
auto main() -> int {
    bool passed{ true };
    passed &= run(test_latch_and_coalesce);
    passed &= run(test_wakeups);
    passed &= run(test_close_and_reset);
    passed &= run(test_wait_for_timeout);
    passed &= run(test_when_all_order);
    passed &= test_query_params();
    passed &= test_url_coding();
    passed &= test_headers();
    passed &= test_form_data();
    passed &= test_error_codes();
    std::println("test_util: {}", passed ? "ok" : "FAILED");
    return passed ? 0 : 1;
}
