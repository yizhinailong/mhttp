/**
 * @file client_stream_test.cpp
 * @brief Verify awaited client receivers, progress, redirect filtering, callback deadlines, and cancellation.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Start a public loopback listener with a dynamic port.
 * @param server Test-owned listener with a running longer-lived runtime.
 * @return Actual bound port, or an exception on fixture failure.
 */
auto start_client_stream_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start client-stream listener" }; }
    return *port;
}

/**
 * @brief Observe callback frame destruction after success, exceptions, deadlines, and stop.
 */
struct CallbackFrame {
    std::atomic<unsigned>& active; ///< Counter outliving the callback and its client.
    /**
     * @brief Register active user work.
     * @param count Longer-lived counter.
     */
    explicit CallbackFrame(std::atomic<unsigned>& count) : active{ count } { ++active; }
    /**
     * @brief Release after nested awaits have drained.
     */
    ~CallbackFrame() { --active; }
};

/**
 * @brief Deliver a prefix before the server can finish producing and preserve trailers and socket reuse.
 */
auto client_incremental_receiver() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start incremental receiver runtime");
    std::atomic<bool> released{}, produced{};
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Get>("/stream", [&](auto&, auto& response) {
        Check(response.SetChunkedContentProvider([&](auto& writer) -> mhttp::Task<mhttp::Result<void>> {
            auto written = co_await writer.Write(std::string("a\0b", 3));
            if (!written) { co_return written; }
            while (!released.load()) { co_await mhttp::Sleep(1ms); }
            written = co_await writer.Write(std::string(257, 'x'));
            if (!written) { co_return written; }
            produced = true;
            mhttp::Headers trailers;
            Check(trailers.Add("X-Sum", "260").has_value(), "streamed response trailer");
            co_return co_await writer.Finish(std::move(trailers));
        }).has_value(), "configure gated response");
    }).has_value(), "register incremental response");
    Check(server.set_http_handler<mhttp::method::Get>("/next", [](auto&, auto& response) { static_cast<void>(response.SetContent("next", "text/plain")); }).has_value(),
        "register buffered successor");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_client_stream_server(server), { .response_chunk_bytes = 2 } };
    std::vector<mhttp::client::TransferProgress> samples;
    std::string body;
    unsigned heads{};
    mhttp::client::TransferCallbacks callbacks;
    callbacks.on_headers = [&](auto const& head) -> mhttp::Task<mhttp::Result<void>> {
        ++heads;
        Check(head.Status() == mhttp::Status::Ok && head.Body().empty() && head.Trailers().Empty() && !head.KeepAlive(),
            "headers snapshot precedes body validation and has no premature reuse permission");
        auto busy = co_await client.Get("/next");
        Check(!busy && busy.error() == mhttp::Error::Busy, "same-client callback reentrancy is rejected");
        co_return mhttp::Result<void>{};
    };
    callbacks.on_body = [&, owned = std::make_unique<unsigned>(2)](std::string bytes) -> mhttp::Task<mhttp::Result<void>> {
        Check(!bytes.empty() && bytes.size() <= *owned, "owned decoded chunks respect receiver bound");
        if (body.empty()) {
            Check(!produced.load(), "receiver runs while the producer is still gated");
            released = true;
        }
        auto copy = bytes;
        co_await mhttp::Sleep(1ms);
        CheckEq(bytes, copy, "owned callback bytes survive suspension");
        body += bytes;
        co_return mhttp::Result<void>{};
    };
    callbacks.on_progress = [&](auto sample) -> mhttp::Task<mhttp::Result<void>> {
        samples.push_back(sample);
        co_return mhttp::Result<void>{};
    };
    auto reply = mhttp::SyncWait(runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "/stream" }, std::move(callbacks)));
    Check(reply && reply->Body().empty() && reply->Trailers().Get("X-Sum") == "260", "streaming returns owned final trailers without aggregate body");
    Check(body == std::string("a\0b", 3) + std::string(257, 'x') && heads == 1, "all decoded binary content reaches receiver once");
    std::uint64_t previous{};
    unsigned complete{};
    for (auto const& sample : samples) {
        if (sample.direction != mhttp::client::TransferDirection::Download) { continue; }
        Check(!sample.total && sample.transferred >= previous && sample.hop == 0, "chunked download progress has no invented total");
        previous = sample.transferred;
        if (sample.complete) { ++complete; }
    }
    Check(previous == 260 && complete == 1, "one complete decoded download sample follows terminal framing");
    auto next = mhttp::SyncWait(runtime, client.Get("/next"));
    Check(next && next->Body() == "next" && next->LocalEndpoint() == reply->LocalEndpoint(), "complete streamed response permits socket reuse");
    Check(server.Stop().has_value(), "stop first receiver fixture");
}

/**
 * @brief Filter intermediate redirect bodies while reporting upload and final download progress by hop.
 */
auto client_stream_redirects_and_upload_progress() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start receiver redirect runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Post>("/start", [](auto& request, auto& response) {
        CheckEq(request.Body(), "upload", "initial buffered upload");
        static_cast<void>(response.SetRedirect("/final", mhttp::Status::TemporaryRedirect));
        static_cast<void>(response.SetContent("hidden redirect body", "text/plain"));
    }).has_value(), "register preserved-body redirect");
    Check(server.set_http_handler<mhttp::method::Post>("/final", [](auto& request, auto& response) {
        CheckEq(request.Body(), "upload", "307 replays buffered upload");
        static_cast<void>(response.SetContent("final", "text/plain"));
    }).has_value(), "register final response");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_client_stream_server(server),
        { .max_redirects = 2, .response_chunk_bytes = 2, .upload_chunk_bytes = 2 } };
    unsigned heads{};
    std::string body;
    std::vector<mhttp::client::TransferProgress> samples;
    mhttp::client::TransferCallbacks callbacks;
    callbacks.on_headers = [&](auto const& head) -> mhttp::Task<mhttp::Result<void>> {
        ++heads;
        CheckEq(head.Status(), mhttp::Status::Ok, "only final head reaches callback");
        co_return mhttp::Result<void>{};
    };
    callbacks.on_body = [&](std::string bytes) -> mhttp::Task<mhttp::Result<void>> { body += bytes; co_return mhttp::Result<void>{}; };
    callbacks.on_progress = [&](auto sample) -> mhttp::Task<mhttp::Result<void>> { samples.push_back(sample); co_return mhttp::Result<void>{}; };
    mhttp::client::Request request{ mhttp::Method::Post, "/start" };
    Check(request.SetContent("upload").has_value(), "own replayable body");
    auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request), std::move(callbacks)));
    Check(reply && reply->Body().empty() && body == "final" && heads == 1, "redirect content never pollutes streamed final response");
    for (std::size_t hop{}; hop != 2; ++hop) {
        std::uint64_t previous{};
        unsigned segments{}, completed{};
        for (auto const& sample : samples) {
            if (sample.direction != mhttp::client::TransferDirection::Upload || sample.hop != hop) { continue; }
            Check(sample.transferred >= previous && sample.total == 6, "upload progress is monotonic within each hop");
            previous = sample.transferred;
            if (sample.transferred && !sample.complete) { ++segments; }
            if (sample.complete) { ++completed; }
        }
        Check(previous == 6 && segments == 3 && completed == 1, "each replay reports bounded upload segments and one completion");
    }
    Check(std::ranges::none_of(samples, [](auto sample) {
        return sample.direction == mhttp::client::TransferDirection::Download && sample.hop != 1;
    }), "followed redirect bodies have no download progress callbacks");
    Check(server.Stop().has_value(), "drain receiver redirects");
}

/**
 * @brief Contain callback errors and exceptions, cancel suspended callbacks on their transfer deadline, and reconnect.
 */
auto client_stream_callback_failures() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start callback failure runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Get>("/body", [](auto&, auto& response) { static_cast<void>(response.SetContent("body", "text/plain")); }).has_value(),
        "register callback failure input");
    auto const port = start_client_stream_server(server);
    std::atomic<unsigned> active{};
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port, { .read_timeout = 100ms, .write_timeout = 100ms } };
    for (unsigned mode{}; mode != 4; ++mode) {
        mhttp::client::TransferCallbacks callbacks;
        auto callback = [&, mode](auto) -> mhttp::Task<mhttp::Result<void>> {
            CallbackFrame frame{ active };
            if (mode == 0) { co_return std::unexpected{ mhttp::Error::Canceled }; }
            if (mode == 1) { throw std::runtime_error{ "callback failure" }; }
            co_await mhttp::Sleep(20s);
            co_return mhttp::Result<void>{};
        };
        if (mode == 3) { callbacks.on_progress = std::move(callback); }
        else { callbacks.on_body = std::move(callback); }
        auto failed = mhttp::SyncWait(runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "/body" }, std::move(callbacks)));
        auto const expected = mode == 0 ? mhttp::Error::Canceled : mode == 1 ? mhttp::Error::Unknown
            : mode == 3 ? mhttp::Error::WriteTimeout : mhttp::Error::ReadTimeout;
        Check(!failed && failed.error() == expected && active == 0 && !client.IsOpen(), "failed callback drains its frame and closes incomplete transfer");
        auto recovered = mhttp::SyncWait(runtime, client.Get("/body"));
        Check(recovered && recovered->Body() == "body", "callback failure releases Busy and permits reconnect");
    }
    Check(server.Stop().has_value(), "drain callback failure listener");
}

/**
 * @brief Cancel an awaited callback even when no client socket operation is pending.
 */
auto client_stream_callback_stop() -> void {
    for (bool stop_runtime : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start stopped callback runtime");
        mhttp::server::HttpServer server{ runtime };
        Check(server.set_http_handler<mhttp::method::Get>("/body", [](auto&, auto& response) { static_cast<void>(response.SetContent("body", "text/plain")); }).has_value(), "callback stop input");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_client_stream_server(server),
            { .timeout_policy = mhttp::client::TimeoutPolicy::Never } };
        std::latch entered{ 1 };
        std::atomic<unsigned> active{};
        mhttp::client::TransferCallbacks callbacks;
        callbacks.on_body = [&](std::string) -> mhttp::Task<mhttp::Result<void>> {
            CallbackFrame frame{ active };
            entered.count_down();
            co_await mhttp::Sleep(20s);
            co_return mhttp::Result<void>{};
        };
        mhttp::Result<mhttp::client::Response> result = std::unexpected{ mhttp::Error::Unknown };
        std::jthread caller{ [&] {
            result = mhttp::SyncWait(runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "/body" }, std::move(callbacks)));
        } };
        entered.wait();
        if (stop_runtime) { runtime.Stop(); }
        else { client.Stop(); }
        caller.join();
        Check(!result && result.error() == mhttp::Error::Canceled && active == 0, "explicit/runtime stop cancels and drains receiver awaits");
        Check(server.Stop().has_value(), "drain callback cancellation listener");
    }
}

/**
 * @brief Propagate cancellation from a server handler calling this client while its runtime remains active.
 */
auto client_stream_caller_cancellation() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start inherited-cancellation runtime");
    mhttp::server::HttpServer upstream{ runtime };
    Check(upstream.set_http_handler<mhttp::method::Get>("/body", [](auto&, auto& response) { static_cast<void>(response.SetContent("body", "text/plain")); }).has_value(),
        "inherited-cancellation upstream");
    mhttp::client::HttpClient outbound{ runtime, "127.0.0.1", start_client_stream_server(upstream),
        { .timeout_policy = mhttp::client::TimeoutPolicy::Never } };
    std::atomic<unsigned> active{};
    std::latch entered{ 1 };
    mhttp::server::HttpServer proxy{ runtime };
    Check(proxy.set_http_handler<mhttp::method::Get>("/proxy", [&](auto&, auto&) -> mhttp::Task<void> {
        mhttp::client::TransferCallbacks callbacks;
        callbacks.on_body = [&](std::string) -> mhttp::Task<mhttp::Result<void>> {
            CallbackFrame frame{ active };
            entered.count_down();
            co_await mhttp::Sleep(20s);
            co_return mhttp::Result<void>{};
        };
        static_cast<void>(co_await outbound.Send(mhttp::client::Request{ mhttp::Method::Get, "/body" }, std::move(callbacks)));
    }).has_value(), "proxy handler owns nested client operation");
    mhttp::client::HttpClient caller{ runtime, "127.0.0.1", start_client_stream_server(proxy) };
    mhttp::Result<mhttp::client::Response> result = std::unexpected{ mhttp::Error::Unknown };
    std::jthread worker{ [&] { result = mhttp::SyncWait(runtime, caller.Get("/proxy")); } };
    entered.wait();
    Check(proxy.Stop().has_value(), "cancel only the proxy session and drain its nested client callback");
    worker.join();
    Check(!result && active == 0 && !outbound.IsOpen(), "caller cancellation closes nested transfer without stopping its runtime");
    auto recovered = mhttp::SyncWait(runtime, outbound.Get("/body"));
    Check(recovered && recovered->Body() == "body", "inherited cancellation releases Busy and preserves a running client runtime");
    Check(upstream.Stop().has_value(), "drain inherited-cancellation upstream");
}

/**
 * @brief Run bounded client streaming scenarios.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Process status.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client incremental receiver", client_incremental_receiver },
        { "client stream redirects and upload progress", client_stream_redirects_and_upload_progress },
        { "client callback failures", client_stream_callback_failures },
        { "client callback stop", client_stream_callback_stop },
        { "client inherited caller cancellation", client_stream_caller_cancellation },
    });
}
