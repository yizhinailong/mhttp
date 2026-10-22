/**
 * @file client_provider_test.cpp
 * @brief Verify bounded provider uploads, replay, Expect rejection, total limits, deadlines, and cleanup.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Start a provider listener on a dynamically assigned loopback port.
 * @param server Listener with a running longer-lived runtime.
 * @return Port, or an exception on fixture failure.
 */
auto start_provider_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start provider listener" }; }
    return *port;
}

/**
 * @brief Stream more decoded data than a single write permits, preserving framing, exact limits, and persistence.
 */
auto client_provider_binary_and_empty() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start provider runtime");
    mhttp::server::HttpServer server{ runtime, { .request_chunk_bytes = 7 } };
    Check(server.set_lazy_http_handler<mhttp::method::Post>("/echo", [](auto& request, auto& response) -> mhttp::Task<void> {
        std::string body;
        for (;;) {
            auto chunk = co_await request.ReadBody();
            Check(chunk.has_value(), "decode streamed upload");
            if (!*chunk) { break; }
            body += **chunk;
        }
        Check(request.Body().empty(), "server does not aggregate lazy input");
        static_cast<void>(response.Headers().Set("X-Framing", request.Headers().Has("Transfer-Encoding") ? "chunked" : "fixed"));
        static_cast<void>(response.SetContent(std::move(body), "application/octet-stream"));
    }).has_value(), "register provider echo");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_provider_server(server),
        { .max_request_bytes = 512, .upload_chunk_bytes = 64, .upload_body_bytes = 1313 } };
    std::string source(1313, '\0');
    for (std::size_t index{}; index != source.size(); ++index) { source[index] = static_cast<char>(index); }
    std::optional<mhttp::Endpoint> endpoint;
    for (bool known : { true, false }) {
        unsigned calls{};
        std::vector<mhttp::client::TransferProgress> samples;
        mhttp::client::Request request{ mhttp::Method::Post, "/echo" };
        Check(request.Headers().Set("Content-Length", "999").has_value(), "stale upload length");
        Check(request.Headers().Set("Transfer-Encoding", "gzip").has_value(), "stale upload encoding");
        Check(request.SetContentProvider(known ? std::optional<std::uint64_t>{ source.size() } : std::nullopt,
            [owned = std::make_unique<std::string>(source), &calls](std::uint64_t offset, std::size_t maximum) -> mhttp::Task<mhttp::Result<std::string>> {
                ++calls;
                Check(mhttp::Runtime::IsIoThread() && maximum > 0 && maximum <= 64, "provider inherits runtime and positive bounded demand");
                auto bytes = owned->substr(static_cast<std::size_t>(offset), maximum);
                co_return co_await mhttp::Offload([bytes = std::move(bytes)] { return bytes; });
            }).has_value(), "own binary provider");
        Check(request.IsStreaming() && request.Body().empty() && calls == 0, "provider construction is lazy");
        CheckEq(request.ContentLength(), known ? std::optional<std::uint64_t>{ source.size() } : std::nullopt, "declared provider length");
        mhttp::client::TransferCallbacks callbacks;
        callbacks.on_progress = [&](auto sample) -> mhttp::Task<mhttp::Result<void>> { samples.push_back(sample); co_return mhttp::Result<void>{}; };
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request), std::move(callbacks)));
        Check(reply && reply->Body() == source && reply->Header("X-Framing") == (known ? "fixed" : "chunked"), "provider streams authoritative framing beyond single-write bound");
        if (endpoint) { CheckEq(reply->LocalEndpoint(), *endpoint, "fixed/chunked uploads reuse fully consumed connection"); }
        endpoint = reply->LocalEndpoint();
        CheckEq(calls, known ? 21u : 22u, "known length avoids extra invocation; unknown length probes EOF at exact total bound");
        unsigned complete{};
        for (auto const& sample : samples) {
            if (sample.direction != mhttp::client::TransferDirection::Upload) { continue; }
            CheckEq(sample.total, known ? std::optional<std::uint64_t>{ source.size() } : std::nullopt, "upload progress preserves known/unknown total");
            if (sample.complete) { ++complete; CheckEq(sample.transferred, source.size(), "completed upload count"); }
        }
        CheckEq(complete, 1u, "terminal framing produces one upload completion");
    }
    for (bool known : { true, false }) {
        unsigned calls{};
        mhttp::client::Request request{ mhttp::Method::Post, "/echo" };
        Check(request.SetContentProvider(known ? std::optional<std::uint64_t>{ 0 } : std::nullopt,
            [&](auto, auto) -> mhttp::Task<mhttp::Result<std::string>> { ++calls; co_return std::string{}; }).has_value(), "empty provider");
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(reply && reply->Body().empty() && reply->LocalEndpoint() == endpoint, "empty fixed/chunked provider preserves pipeline boundary");
        CheckEq(calls, known ? 0u : 1u, "known zero length does not invoke provider");
    }
    Check(server.Stop().has_value(), "drain provider echo");
}

/**
 * @brief Replay retained providers from offset zero on 307/308 and clear them on 303; suppress early-final uploads.
 */
auto client_provider_redirects_and_expect() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start replay provider runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Post>("/redirect/:code", [](auto& request, auto& response) {
        CheckEq(request.Body(), "provider", "initial redirected provider upload");
        static_cast<void>(response.SetRedirect("/final", static_cast<mhttp::Status>(*request.template PathParam<unsigned>("code"))));
    }).has_value(), "provider redirect route");
    Check(server.set_http_handler<mhttp::method::Post>("/final", [](auto& request, auto& response) {
        CheckEq(request.Body(), "provider", "preserved method replays provider");
        static_cast<void>(response.SetContent("post", "text/plain"));
    }).has_value(), "provider replay target");
    Check(server.set_http_handler<mhttp::method::Get>("/final", [](auto& request, auto& response) {
        Check(request.Body().empty() && !request.Headers().Has("Expect"), "303 clears provider and Expect");
        static_cast<void>(response.SetContent("get", "text/plain"));
    }).has_value(), "provider rewrite target");
    Check(server.set_lazy_http_handler<mhttp::method::Post>("/reject", [](auto&, auto& response) {
        static_cast<void>(response.SetError(mhttp::Status::Forbidden));
    }).has_value(), "early final rejects provider without interim response");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_provider_server(server),
        { .max_redirects = 2, .expect_continue_threshold = 1, .upload_chunk_bytes = 3 } };
    for (bool known : { true, false }) {
        for (unsigned code : { 303u, 307u, 308u }) {
            std::vector<std::uint64_t> offsets;
            mhttp::client::Request request{ mhttp::Method::Post, std::format("/redirect/{}", code) };
            Check(request.SetContentProvider(known ? std::optional<std::uint64_t>{ 8 } : std::nullopt,
                [&](auto offset, auto count) -> mhttp::Task<mhttp::Result<std::string>> {
                    offsets.push_back(offset);
                    co_return std::string{ "provider" }.substr(static_cast<std::size_t>(offset), count);
                }).has_value(), "replayable provider");
            auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
            Check(reply && reply->Body() == (code == 303 ? "get" : "post"), "provider redirect semantics match buffered requests");
            CheckEq(std::ranges::count(offsets, std::uint64_t{}), code == 303 ? 1 : 2, "only preserving redirects restart at offset zero");
        }
        unsigned calls{}, upload_samples{};
        mhttp::client::Request rejected{ mhttp::Method::Post, "/reject" };
        Check(rejected.SetContentProvider(known ? std::optional<std::uint64_t>{ 8 } : std::nullopt,
            [&](auto, auto) -> mhttp::Task<mhttp::Result<std::string>> { ++calls; co_return "provider"; }).has_value(), "deferred rejected upload");
        mhttp::client::TransferCallbacks callbacks;
        callbacks.on_progress = [&](auto sample) -> mhttp::Task<mhttp::Result<void>> {
            if (sample.direction == mhttp::client::TransferDirection::Upload) { ++upload_samples; }
            co_return mhttp::Result<void>{};
        };
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(rejected), std::move(callbacks)));
        Check(reply && reply->Status() == mhttp::Status::Forbidden && !reply->KeepAlive() && calls == 0 && upload_samples == 0,
            "final response before 100 leaves provider and upload callbacks unconsumed");
    }
    Check(server.Stop().has_value(), "drain provider replay listener");
}

/**
 * @brief Validate providers transactionally and contain premature EOF, oversized chunks, total overflow, and exceptions.
 */
auto client_provider_failures() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start failed provider runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Post>("/body", [](auto&, auto&) {}).has_value(), "failed upload route");
    Check(server.set_http_handler<mhttp::method::Get>("/ok", [](auto&, auto& response) { static_cast<void>(response.SetContent("ok", "text/plain")); }).has_value(), "provider recovery route");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_provider_server(server), { .upload_chunk_bytes = 3, .upload_body_bytes = 5 } };
    for (unsigned mode{}; mode != 6; ++mode) {
        unsigned calls{};
        mhttp::client::Request request{ mhttp::Method::Post, "/body" };
        auto const length = mode == 4 ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{ mode == 5 ? 6u : 3u };
        Check(request.SetContentProvider(length, [&, mode](auto, auto count) -> mhttp::Task<mhttp::Result<std::string>> {
            ++calls;
            if (mode == 0) { co_return std::unexpected{ mhttp::Error::InvalidRequest }; }
            if (mode == 1) { throw std::runtime_error{ "provider failure" }; }
            if (mode == 2) { co_return std::string(count + 1, 'x'); }
            if (mode == 3) { co_return std::string{}; }
            co_return std::string(3, 'x');
        }).has_value(), "configure failed provider");
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        auto expected = mode == 0 || mode == 3 ? mhttp::Error::InvalidRequest
            : mode == 1 ? mhttp::Error::Unknown : mhttp::Error::BodyTooLarge;
        Check(!result && result.error() == expected && (mode == 5 || !client.IsOpen()), "failed provider closes incomplete upload");
        if (mode == 5) { Check(client.IsOpen(), "preflight size rejection preserves an existing idle connection"); }
        Check(mode != 5 || calls == 0, "declared upload total is rejected before invoking provider");
        auto recovered = mhttp::SyncWait(runtime, client.Get("/ok"));
        Check(recovered && recovered->Body() == "ok", "failed upload releases Busy and reconnects");
    }
    mhttp::client::Request request;
    Check(request.SetContent("original", "text/plain").has_value(), "initial buffered representation");
    Check(!request.SetContentProvider(1, {}) && request.Body() == "original" && !request.IsStreaming(), "empty provider preserves buffer");
    auto provider = [](auto, auto) -> mhttp::Task<mhttp::Result<std::string>> { co_return "x"; };
    Check(!request.SetContentProvider(1, provider, "invalid\r\nvalue") && request.Body() == "original", "invalid media type preserves prior state");
    Check(request.SetContentProvider(1, provider).has_value() && request.IsStreaming(), "replace buffer with provider");
    Check(request.SetContent("buffer").has_value() && !request.IsStreaming() && request.ContentLength() == 6, "buffered setter clears provider");
    Check(server.Stop().has_value(), "drain failed provider listener");
}

/**
 * @brief Apply phase/overall budgets across provider awaits while Step renews and Never disables timing.
 */
auto client_provider_timeout_policies() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start provider deadline runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Post>("/body", [](auto& request, auto& response) {
        static_cast<void>(response.SetContent(request.Body(), "text/plain"));
    }).has_value(), "provider deadline echo");
    auto const port = start_provider_server(server);
    for (auto policy : { mhttp::client::TimeoutPolicy::Phase, mhttp::client::TimeoutPolicy::Overall,
             mhttp::client::TimeoutPolicy::Step, mhttp::client::TimeoutPolicy::Never }) {
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", port,
            { .write_timeout = 100ms, .timeout_policy = policy, .timeout = 100ms, .upload_chunk_bytes = 2 } };
        mhttp::client::Request request{ mhttp::Method::Post, "/body" };
        Check(request.SetContentProvider(6, [](auto, auto count) -> mhttp::Task<mhttp::Result<std::string>> {
            co_await mhttp::Sleep(60ms);
            co_return std::string(count, 'x');
        }).has_value(), "delayed bounded provider");
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        if (policy == mhttp::client::TimeoutPolicy::Step || policy == mhttp::client::TimeoutPolicy::Never) {
            Check(result && result->Body() == "xxxxxx", "Step renews provider budgets; Never permits application waits");
        } else { Check(!result && result.error() == mhttp::Error::WriteTimeout, "provider awaits share one phase/overall budget"); }
    }
    Check(server.Stop().has_value(), "drain provider deadline listener");
}

/**
 * @brief Cancel suspended provider work without depending on an in-flight network operation.
 */
auto client_provider_stop() -> void {
    for (bool stop_runtime : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start canceled provider runtime");
        mhttp::server::HttpServer server{ runtime };
        Check(server.set_http_handler<mhttp::method::Post>("/body", [](auto&, auto&) {}).has_value(), "pending provider upload");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_provider_server(server),
            { .timeout_policy = mhttp::client::TimeoutPolicy::Never } };
        std::atomic<unsigned> active{};
        std::latch entered{ 1 };
        mhttp::client::Request request{ mhttp::Method::Post, "/body" };
        Check(request.SetContentProvider(3, [&](auto, auto) -> mhttp::Task<mhttp::Result<std::string>> {
            ++active;
            /**
             * @brief Observe provider frame cleanup on every completion path.
             */
            struct ProviderFrame {
                std::atomic<unsigned>& count; ///< Counter outliving the Send operation.
                /**
                 * @brief Release after nested work has drained.
                 */
                ~ProviderFrame() { --count; }
            } frame{ active };
            entered.count_down();
            co_await mhttp::Sleep(20s);
            co_return "abc";
        }).has_value(), "defer cancelable provider");
        mhttp::Result<mhttp::client::Response> result = std::unexpected{ mhttp::Error::Unknown };
        std::jthread caller{ [&] { result = mhttp::SyncWait(runtime, client.Send(std::move(request))); } };
        entered.wait();
        if (stop_runtime) { runtime.Stop(); }
        else { client.Stop(); }
        caller.join();
        Check(!result && result.error() == mhttp::Error::Canceled && active == 0, "Stop cancels provider await and drains its frame");
        Check(server.Stop().has_value(), "drain canceled upload");
    }
}

/**
 * @brief Fit exactly one maximum wire segment for fixed/chunked uploads and enforce the advertised raw capacity.
 */
auto client_provider_framed_bounds() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start framed provider runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.set_http_handler<mhttp::method::Post>("/body", [](auto& request, auto& response) {
        static_cast<void>(response.SetContent(std::to_string(request.Body().size()), "text/plain"));
    }).has_value(), "framed provider byte counter");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_provider_server(server), { .max_request_bytes = 512 } };
    for (bool known : { true, false }) {
        auto const expected = known ? std::size_t{ 512 } : std::size_t{ 505 };
        mhttp::client::Request request{ mhttp::Method::Post, "/body" };
        Check(request.SetContentProvider(known ? std::optional<std::uint64_t>{ expected } : std::nullopt,
            [expected](auto offset, auto maximum) -> mhttp::Task<mhttp::Result<std::string>> {
                CheckEq(maximum, expected, "provider demand reserves exact chunk framing overhead");
                co_return offset == 0 ? std::string(maximum, 'x') : std::string{};
            }).has_value(), "provider at exact wire bound");
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(result && result->Body() == std::to_string(expected), "raw fixed 512 or chunked 505 plus seven framing bytes fits");
    }
    mhttp::client::Request oversized{ mhttp::Method::Post, "/body" };
    Check(oversized.SetContentProvider(std::nullopt, [](auto, auto maximum) -> mhttp::Task<mhttp::Result<std::string>> {
        co_return std::string(maximum + 1, 'x');
    }).has_value(), "provider ignores framed capacity");
    auto failed = mhttp::SyncWait(runtime, client.Send(std::move(oversized)));
    Check(!failed && failed.error() == mhttp::Error::BodyTooLarge && !client.IsOpen(), "one byte beyond framed capacity aborts upload");
    Check(server.Stop().has_value(), "drain framed providers");
}

/**
 * @brief Run bounded provider upload scenarios.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Process status.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "provider binary and empty uploads", client_provider_binary_and_empty },
        { "provider redirect replay and Expect", client_provider_redirects_and_expect },
        { "provider failure containment", client_provider_failures },
        { "provider timeout policies", client_provider_timeout_policies },
        { "provider stop cleanup", client_provider_stop },
        { "provider exact framed bounds", client_provider_framed_bounds },
    });
}
