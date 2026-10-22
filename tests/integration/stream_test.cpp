/**
 * @file stream_test.cpp
 * @brief Verify deferred binary streams, body suppression, terminal failures, and cooperative shutdown.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Count live provider frames through normal return, exception, timeout, and cancellation.
 */
struct ProviderLease {
    std::atomic<unsigned>& active; ///< Counter alive until the server has drained all sessions.
    /**
     * @brief Observe entry into a producer coroutine frame.
     * @param count Longer-lived active frame counter.
     */
    explicit ProviderLease(std::atomic<unsigned>& count) : active{ count } { ++active; }
    /**
     * @brief Observe frame destruction on every coroutine completion path.
     */
    ~ProviderLease() { --active; }
};

/**
 * @brief Start a configured public listener on a dynamically assigned loopback port.
 * @param server Listener whose runtime is already running.
 * @return Actual port, or an exception on fixture failure.
 */
auto start_stream_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start stream listener" }; }
    return *port;
}

/**
 * @brief Send more total bytes than a buffered response permits while retaining bounded segments and persistence.
 */
auto streamed_binary_and_suppression() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start streaming runtime");
    std::atomic<unsigned> providers{}, suppressed{}, active{}, posts{}, accesses{};
    mhttp::server::HttpServer server{ runtime, { .max_response_bytes = 512, .stream_chunk_bytes = 128 } };
    Check(server.SetPostRoutingHandler([&](auto& request, auto& response) {
        ++posts;
        request.Data().Store("post_done", true);
        Check(response.Headers().Set("X-Final", "yes").has_value(), "post hook finishes before head commitment");
    }).has_value(), "configure streaming finalizer");
    Check(server.SetAccessLogger([&](auto const& request, auto const& response, auto) {
        ++accesses;
        Check(response.IsStreaming() && response.Body().empty(), "access observer retains streaming mode without aggregate body");
        CheckEq(response.Header("X-Final"), std::string_view{ "yes" }, "access observes final metadata");
    }).has_value(), "observe only complete streamed responses");
    Check(server.set_http_handler<mhttp::method::Get>("/stream", [&](auto& request, auto& response) {
        Check(response.Headers().Set("Content-Length", "999").has_value(), "stale user framing");
        Check(response.SetChunkedContentProvider([&, owned = std::make_unique<int>(64)](
            mhttp::server::StreamWriter& writer
        ) -> mhttp::Task<mhttp::Result<void>> {
            ProviderLease lease{ active };
            ++providers;
            Check(request.Data().template Fetch<bool>("post_done").value_or(false), "producer begins after final hooks");
            CheckEq(request.Target(), std::string_view{ "/stream" }, "request outlives its deferred producer");
            auto empty = co_await writer.Write({});
            if (!empty) { co_return empty; }
            for (int index{}; index != *owned; ++index) {
                auto written = co_await writer.Write(std::string(128, static_cast<char>(index)));
                if (!written) { co_return written; }
            }
            mhttp::Headers trailers;
            Check(trailers.Add("X-Blocks", "64").has_value(), "owned terminal metadata");
            co_return co_await writer.Finish(std::move(trailers));
        }, "application/octet-stream").has_value(), "defer a move-only binary producer");
    }).has_value(), "register bounded stream");
    Check(server.set_http_handler<mhttp::method::Get>("/empty/:code", [&](auto& request, auto& response) {
        Check(response.SetStatus(static_cast<mhttp::Status>(*request.template PathParam<unsigned>("code"))).has_value(),
            "select bodyless status");
        Check(response.SetChunkedContentProvider([&](auto&) -> mhttp::Task<mhttp::Result<void>> {
            ++suppressed;
            co_return mhttp::Result<void>{};
        }).has_value(), "configure suppressed provider");
    }).has_value(), "register bodyless stream cases");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_stream_server(server) };
    std::string expected;
    for (unsigned index{}; index != 64; ++index) { expected.append(128, static_cast<char>(index)); }
    std::optional<mhttp::Endpoint> connection;
    for (unsigned repeat{}; repeat != 3; ++repeat) {
        auto reply = mhttp::SyncWait(runtime, client.Get("/stream"));
        if (!Check(reply && reply->KeepAlive(), "complete binary stream keeps its connection")) { continue; }
        CheckEq(reply->Body(), expected, "all streamed binary bytes survive without a bounded aggregate response");
        CheckEq(reply->Header("Transfer-Encoding"), std::string_view{ "chunked" }, "authoritative stream coding");
        Check(!reply->Headers().Has("Content-Length"), "stale aggregate length never reaches the wire");
        CheckEq(reply->Trailers().Get("X-Blocks"), std::string_view{ "64" }, "terminal trailers remain separate");
        if (connection) { CheckEq(reply->LocalEndpoint(), *connection, "completed streams reuse one socket"); }
        connection = reply->LocalEndpoint();
    }
    for (auto method : { mhttp::Method::Head, mhttp::Method::Get }) {
        for (auto code : { 204u, 304u }) {
            auto reply = mhttp::SyncWait(runtime, client.Send(
                mhttp::client::Request{ method, std::format("/empty/{}", code) }));
            Check(reply && reply->Body().empty() && reply->KeepAlive(), "bodyless responses have no chunks or terminator");
        }
    }
    auto head = mhttp::SyncWait(runtime, client.Head("/stream"));
    Check(head && head->Body().empty() && head->KeepAlive(), "HEAD advertises stream framing without running a producer");
    auto after_head = mhttp::SyncWait(runtime, client.Get("/stream"));
    Check(after_head && after_head->Body() == expected, "HEAD leaves no terminal chunk ahead of the next response");
    Check(server.Stop().has_value(), "drain streamed response callbacks");
    CheckEq(providers.load(), 4u, "only actual streaming GET requests invoke providers");
    CheckEq(suppressed.load(), 0u, "HEAD/204/304 never invoke suppressed producers");
    CheckEq(active.load(), 0u, "all producer frames have been destroyed");
    CheckEq(posts.load(), 9u, "final hooks run once for every stream or suppressed response");
    CheckEq(accesses.load(), 9u, "access observes completed streamed or suppressed responses once");
}

/**
 * @brief Treat ignored transfer errors, provider failures, and a stalled provider as incomplete responses.
 */
auto streamed_failures() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start failed stream runtime");
    std::atomic<unsigned> active{}, accesses{};
    std::mutex logs_mutex;
    std::vector<mhttp::server::LogRecord> logs;
    mhttp::server::HttpServer server{ runtime,
        { .write_timeout = 100ms, .max_response_bytes = 1024, .stream_chunk_bytes = 128 } };
    Check(server.SetLogger([&](auto const& record) { std::lock_guard lock{ logs_mutex }; logs.push_back(record); }).has_value(),
        "retain failed stream diagnostics");
    Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++accesses; }).has_value(), "observe only completed responses");
    Check(server.set_http_handler<mhttp::method::Get>("/fail/:mode", [&](auto& request, auto& response) {
        auto mode = std::string{ request.PathParam("mode") };
        Check(response.SetChunkedContentProvider([&, mode = std::move(mode)](auto& writer) -> mhttp::Task<mhttp::Result<void>> {
            ProviderLease lease{ active };
            auto prefix = co_await writer.Write("prefix");
            if (!prefix) { co_return prefix; }
            if (mode == "error") { co_return std::unexpected{ mhttp::Error::InvalidRequest }; }
            if (mode == "throw") { throw std::runtime_error{ "private provider failure" }; }
            if (mode == "size") {
                auto oversized = co_await writer.Write(std::string(129, 'x'));
                Check(!oversized && oversized.error() == mhttp::Error::BodyTooLarge, "chunk limit is checked before encoding");
            } else { co_await mhttp::Sleep(2s); }
            co_return mhttp::Result<void>{};
        }).has_value(), "configure post-commit failure");
    }).has_value(), "register failed streams");
    Check(server.set_http_handler<mhttp::method::Get>("/ok", [](auto&, auto& response) { Check(response.SetContent("ok", "text/plain").has_value(), "fresh response"); }).has_value(),
        "register reconnect probe");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_stream_server(server),
        { .timeout_policy = mhttp::client::TimeoutPolicy::Overall, .timeout = 2s } };
    for (auto mode : { "error", "throw", "size", "stall" }) {
        auto reply = mhttp::SyncWait(runtime, client.Get(std::string{ "/fail/" } + mode));
        Check(!reply && !client.IsOpen(), "incomplete stream is never accepted or reused");
        auto fresh = mhttp::SyncWait(runtime, client.Get("/ok"));
        Check(fresh && fresh->Body() == "ok", "next request reconnects after a streaming failure");
    }
    Check(server.Stop().has_value(), "drain failed providers and deadline cancellation");
    CheckEq(active.load(), 0u, "throwing and canceled provider frames are released");
    CheckEq(accesses.load(), 4u, "partial streams do not count as complete access responses");
    std::lock_guard lock{ logs_mutex };
    for (auto error : { mhttp::Error::InvalidRequest, mhttp::Error::Unknown, mhttp::Error::BodyTooLarge, mhttp::Error::WriteTimeout }) {
        CheckEq(std::ranges::count_if(logs, [error](auto const& record) {
            return record.event == mhttp::server::LogEvent::SessionFailure && record.error == error;
        }), 1, "each failed stream retains its original diagnostic classification once");
    }
}

/**
 * @brief Cancel a suspended producer through both listener and runtime stop and drain its frame.
 */
auto streamed_shutdown() -> void {
    for (bool stop_runtime : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start canceled stream runtime");
        std::atomic<unsigned> active{}, accesses{};
        std::latch entered{ 1 };
        mhttp::server::HttpServer server{ runtime };
        Check(server.set_http_handler<mhttp::method::Get>("/wait", [&](auto&, auto& response) {
            Check(response.SetChunkedContentProvider([&](auto& writer) -> mhttp::Task<mhttp::Result<void>> {
                ProviderLease lease{ active };
                auto prefix = co_await writer.Write("prefix");
                if (!prefix) { co_return prefix; }
                entered.count_down();
                co_await mhttp::Sleep(20s);
                co_return mhttp::Result<void>{};
            }).has_value(), "configure suspended stream");
        }).has_value(), "register canceled producer");
        Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++accesses; }).has_value(), "observe completed streams only");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_stream_server(server) };
        std::atomic<bool> failed{};
        std::jthread caller{ [&] { failed = !mhttp::SyncWait(runtime, client.Get("/wait")); } };
        entered.wait();
        if (stop_runtime) { runtime.Stop(); }
        Check(server.Stop().has_value(), "listener stop drains a canceled producer");
        caller.join();
        Check(failed.load() && active.load() == 0 && accesses.load() == 0, "cancel never finishes a partial stream or leaks its frame");
    }
}

/**
 * @brief Reject retired lazy writes and prevent old timers from closing a subsequent response.
 */
auto streamed_retirement() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start stream retirement runtime");
    std::optional<mhttp::Task<mhttp::Result<void>>> late_write, dropped_write;
    std::latch accessed{ 1 };
    mhttp::server::HttpServer server{ runtime, { .write_timeout = 100ms } };
    Check(server.set_http_handler<mhttp::method::Get>("/stream", [&](auto&, auto& response) {
        Check(response.SetChunkedContentProvider([&](auto& writer) -> mhttp::Task<mhttp::Result<void>> {
            late_write.emplace(writer.Write("must not send"));
            dropped_write.emplace(writer.Write("must not start"));
            std::string bytes{ "complete" };
            auto operation = writer.Write(bytes);
            bytes.assign("caller changed");
            auto written = co_await std::move(operation);
            if (!written) { co_return written; }
            auto finished = co_await writer.Finish();
            if (!finished) { co_return finished; }
            auto extra = co_await writer.Write("after completion");
            auto duplicate = co_await writer.Finish();
            Check(!extra && extra.error() == mhttp::Error::InvalidRequest, "completed writer rejects body bytes");
            Check(!duplicate && duplicate.error() == mhttp::Error::InvalidRequest, "completed writer rejects a second terminator");
            co_return mhttp::Result<void>{};
        }).has_value(), "own pending lazy writes");
    }).has_value(), "register retired write scenario");
    Check(server.set_http_handler<mhttp::method::Get>("/slow", [](auto&, auto& response) -> mhttp::Task<void> {
        co_await mhttp::Sleep(200ms);
        Check(response.SetContent("next", "text/plain").has_value(), "later response after old stream deadline");
    }).has_value(), "register socket reuse after timer retirement");
    Check(server.SetAccessLogger([&](auto const& request, auto const&, auto) {
        if (request.Path() == "/stream") { accessed.count_down(); }
    }).has_value(), "publish provider and stream retirement");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_stream_server(server) };
    auto reply = mhttp::SyncWait(runtime, client.Get("/stream"));
    Check(reply && reply->Body() == "complete", "issued writes own bytes and emit only one terminator");
    accessed.wait();
    auto late = mhttp::SyncWait(runtime, std::move(*late_write));
    Check(!late && late.error() == mhttp::Error::InvalidRequest, "escaped lazy operation cannot write into a later response");
    late_write.reset();
    auto next = mhttp::SyncWait(runtime, client.Get("/slow"));
    Check(reply && next && next->Body() == "next" && next->LocalEndpoint() == reply->LocalEndpoint(),
        "retired timers and writes preserve the reused connection");
    dropped_write.reset();
    Check(server.Stop().has_value(), "drain retired stream state");
}

/**
 * @brief Enforce declared lengths without aggregate buffering and preserve reuse after exact output.
 */
auto streamed_fixed_lengths() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start fixed stream runtime");
    std::atomic<unsigned> calls{};
    mhttp::server::HttpServer server{ runtime, { .max_response_bytes = 512, .stream_chunk_bytes = 64 } };
    Check(server.set_http_handler<mhttp::method::Get>("/fixed/:mode", [&](auto& request, auto& response) {
        auto mode = std::string{ request.PathParam("mode") };
        auto const length = mode == "zero" ? 0u : mode == "short" ? 66u : mode == "long" ? 1u : 1024u;
        Check(response.SetContentProvider(length, [&, mode = std::move(mode)](auto& writer) -> mhttp::Task<mhttp::Result<void>> {
            ++calls;
            if (mode == "zero") { co_return mhttp::Result<void>{}; }
            auto const count = mode == "short" || mode == "long" ? 1 : 16;
            for (int i{}; i < count; ++i) {
                auto written = co_await writer.Write(std::string(64, static_cast<char>(i)));
                if (!written) { co_return written; }
            }
            if (mode == "trailers") {
                mhttp::Headers trailers;
                static_cast<void>(trailers.Set("X-Forbidden", "yes"));
                co_return co_await writer.Finish(std::move(trailers));
            }
            co_return mhttp::Result<void>{};
        }).has_value(), "register exact length source");
    }).has_value(), "register fixed stream route");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_stream_server(server) };
    auto head = mhttp::SyncWait(runtime, client.Send(mhttp::client::Request{ mhttp::Method::Head, "/fixed/exact" }));
    Check(head && head->Header("Content-Length") == "1024" && head->Body().empty() && calls == 0,
        "HEAD carries declared length and never calls source");
    auto full = mhttp::SyncWait(runtime, client.Get("/fixed/exact"));
    Check(full && full->Body().size() == 1024 && full->Header("Content-Length") == "1024"
        && !full->Headers().Has("Transfer-Encoding"), "fixed stream exceeds buffered wire bound with exact framing");
    auto empty = mhttp::SyncWait(runtime, client.Get("/fixed/zero"));
    Check(full && empty && empty->Body().empty() && full->LocalEndpoint() == empty->LocalEndpoint(),
        "exact and zero length streams preserve persistence");
    for (auto mode : { "short", "long" }) {
        auto failed = mhttp::SyncWait(runtime, client.Get(std::string{ "/fixed/" } + mode));
        Check(!failed, "too few or too many bytes close incomplete framing");
        auto recovered = mhttp::SyncWait(runtime, client.Get("/fixed/exact"));
        Check(recovered && recovered->Body().size() == 1024, "length failure cannot corrupt the next response");
    }
    Check(server.Stop().has_value(), "drain fixed streams");
}

/**
 * @brief Run bounded streaming integration scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "stream binary persistence and suppression", streamed_binary_and_suppression },
        { "stream provider errors and deadlines", streamed_failures },
        { "stream listener and runtime shutdown", streamed_shutdown },
        { "stream timer and lazy write retirement", streamed_retirement },
        { "stream exact lengths", streamed_fixed_lengths },
    });
}
