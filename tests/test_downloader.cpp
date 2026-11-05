/**
 * @file test_downloader.cpp
 * @brief Exercise Downloader (resume, retries, redirects, cache, cancel, pause) and DiskCache.
 */
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import mhttp;

#include "support.hpp"

using namespace test_support;
using mhttp::Field;
using mhttp::Status;
using mhttp::Verb;
using mhttp::client::Downloader;
using mhttp::client::HttpClientPool;

namespace {

    auto run_client(std::function<net::awaitable<void>(net::io_context&)> const& body) -> void {
        net::io_context ioc;
        bool finished{ false };
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true; }, net::detached);
        ioc.run();
        check(finished, "the client coroutine must run to completion");
    }

    auto sleep_for(std::chrono::milliseconds duration) -> net::awaitable<void> {
        net::steady_timer timer{ co_await net::this_coro::executor, duration };
        co_await timer.async_wait(net::use_awaitable);
    }

    auto read_file(std::filesystem::path const& path) -> std::string {
        std::ifstream in{ path, std::ios::binary };
        return std::string{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
    }

    /**
     * @brief Requests seen by the test server: "path|Range|If-None-Match".
     */
    struct RequestLog {
        std::mutex mutex;
        std::vector<std::string> entries;

        auto Record(mhttp::Request const& req) -> void {
            std::lock_guard lock{ mutex };
            entries.push_back(std::format("{}|{}|{}", req.Path(), req[Field::range], req[Field::if_none_match]));
        }

        auto Entries() -> std::vector<std::string> {
            std::lock_guard lock{ mutex };
            return entries;
        }

        auto Count(std::string_view path) -> std::size_t {
            std::lock_guard lock{ mutex };
            return static_cast<std::size_t>(std::ranges::count_if(entries, [&](auto const& entry) { return entry.starts_with(std::format("{}|", path)); }));
        }

        auto Clear() -> void {
            std::lock_guard lock{ mutex };
            entries.clear();
        }
    };

    /**
     * @brief File server with redirect, failure, flaky, and slow routes.
     */
    struct Fixture {
        TempDir dir;
        std::string content;
        std::filesystem::path file;
        RequestLog log;
        std::unique_ptr<TestServer> server;

        Fixture() {
            for (int i{ 0 }; i < 20000; ++i) {
                content += static_cast<char>('a' + (i * 7) % 26);
            }
            file = dir.Write("data.bin", content);
            server = std::make_unique<TestServer>([this](mhttp::HttpServer& srv) {
                auto& router{ srv.GetRouter() };
                router.SetHttpHandler(Verb::get, "/file", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetFileContent(file, req.Base());
                });
                router.SetHttpHandler(Verb::get, "/fresh", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetFileContent(file, req.Base());
                    resp.Set(Field::cache_control, "max-age=60");
                });
                router.SetHttpHandler(Verb::get, "/named", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetFileContent(file, req.Base());
                    resp.Set(Field::content_disposition, R"(attachment; filename="../report.txt")");
                });
                router.SetHttpHandler(Verb::get, "/redirect", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetRedirect("file", Status::found);
                });
                router.SetHttpHandler(Verb::get, "/loop", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetRedirect("/loop", Status::found);
                });
                router.SetHttpHandler(Verb::get, "/ignore-range", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetStringContent(content, "application/octet-stream");
                });
                router.SetHttpHandler(Verb::get, "/bad-range", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetStringContent("bad", "application/octet-stream", Status::partial_content);
                });
                router.SetHttpHandler(Verb::get, "/revalidate", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    if (req[Field::if_none_match] == "\"v1\"") {
                        resp.SetEmptyContent(Status::not_modified);
                        resp.Set(Field::cache_control, "max-age=60");
                    } else {
                        resp.SetStringContent(content, "application/octet-stream");
                        resp.Set(Field::cache_control, "max-age=0");
                    }
                    resp.Set(Field::etag, "\"v1\"");
                });
                router.SetHttpHandler(Verb::get, "/vary", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetStringContent(std::string{ req["X-Variant"] }, "text/plain");
                    resp.Set(Field::vary, "X-Variant");
                    resp.Set(Field::cache_control, "max-age=60");
                });
                router.SetHttpHandler(Verb::get, "/missing", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetErrorContent(Status::not_found);
                });
                router.SetHttpHandler(Verb::get, "/busy", [this](mhttp::Request& req, mhttp::Response& resp) {
                    log.Record(req);
                    resp.SetErrorContent(Status::service_unavailable);
                });
                // Without Range: announce the whole file, send half, and drop the connection.
                router.SetHttpHandler(Verb::get, "/flaky", [this](mhttp::Request& req, mhttp::Response& resp) -> net::awaitable<void> {
                    log.Record(req);
                    if (req.Has(Field::range)) {
                        resp.SetFileContent(file, req.Base());
                        co_return;
                    }
                    auto writer{ resp.CreateStreamWriter() };
                    mhttp::Headers headers;
                    headers.Set(Field::content_length, std::to_string(content.size()));
                    co_await writer->WriteHeader(Status::ok, headers, mhttp::StreamWriter::Mode::Relay);
                    co_await writer->WriteBody(net::buffer(content.data(), content.size() / 2), true);
                });
                router.SetHttpHandler(Verb::get, "/slow", [this](mhttp::Request& req, mhttp::Response& resp) -> net::awaitable<void> {
                    log.Record(req);
                    auto writer{ resp.CreateStreamWriter() };
                    boost::system::error_code ec;
                    co_await writer->WriteHeader(Status::ok, mhttp::Headers{}, mhttp::StreamWriter::Mode::Chunked, ec);
                    std::string const chunk(1000, 's');
                    for (int i{ 0 }; i < 20 && !ec; ++i) {
                        net::steady_timer timer{ co_await net::this_coro::executor, std::chrono::milliseconds{ 15 } };
                        co_await timer.async_wait(net::use_awaitable);
                        co_await writer->WriteBody(net::buffer(chunk), i < 19, ec);
                    }
                });
            });
        }

        [[nodiscard]] auto Url(std::string_view path) const -> std::string {
            return std::format("http://127.0.0.1:{}{}", server->Endpoint().port(), path);
        }
    };

    /**
     * @brief Run a downloader test body with a pool and downloader on a fresh context.
     * @param body Receives the downloader.
     */
    auto with_downloader(std::function<net::awaitable<void>(net::io_context&, Downloader&)> const& body) -> void {
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            auto pool{ std::make_shared<HttpClientPool>(ioc.get_executor()) };
            {
                Downloader downloader{ ioc, pool };
                downloader.SetConfig(Downloader::Config{ .max_retries = 2, .retry_backoff = std::chrono::milliseconds{ 10 } });
                co_await body(ioc, downloader);
            }
            pool->Stop();
        });
    }

    auto test_download_and_redirect() -> void {
        Fixture fixture;
        with_downloader([&](net::io_context&, Downloader& downloader) -> net::awaitable<void> {
            std::vector<Downloader::State> states;
            std::uint64_t last_progress{ 0 };
            downloader.SetStateCallback([&](Downloader::State state, boost::system::error_code) { states.push_back(state); });
            downloader.SetProgressCallback([&](Downloader::ProgressInfo const& info) { last_progress = info.downloaded_bytes; });
            auto const target{ fixture.dir.path / "out.bin" };
            auto const ec{ co_await downloader.AsyncDownload(fixture.Url("/file"), target) };
            check(!ec && read_file(target) == fixture.content, std::format("the file must be downloaded whole: {}", ec.message()));
            check(!std::filesystem::exists(target.string() + ".part"), "a completed download must consume its part file");
            check(last_progress == fixture.content.size(), "final progress must report the complete file size");
            check(states == std::vector{ Downloader::State::Connecting, Downloader::State::Downloading, Downloader::State::Completed }, "success must report its state transitions");
            check(downloader.HttpStatus() == Status::ok, "the terminal HTTP status must be available");

            auto redirected{ co_await downloader.AsyncDownload(fixture.Url("/redirect"), target) };
            check(!redirected && read_file(target) == fixture.content, "relative redirects must download their target");
            check(fixture.log.Count("/redirect") == 1 && fixture.log.Count("/file") == 2, "redirects must issue one request per hop");
            auto named{ co_await downloader.AsyncDownload(fixture.Url("/named"), target) };
            check(!named && downloader.SuggestedFilename() == "report.txt", "suggested filenames must discard directory components");
            auto plain{ co_await downloader.AsyncDownload(fixture.Url("/file"), target) };
            check(!plain && downloader.SuggestedFilename().empty(), "a later response must not retain an earlier suggested filename");
        });
    }

    /**
     * @brief Verify existing bytes survive resume, retries, and ignored Range requests.
     */
    auto test_resume_and_retry() -> void {
        Fixture fixture;
        with_downloader([&](net::io_context&, Downloader& downloader) -> net::awaitable<void> {
            auto const target{ fixture.dir.path / "resume.bin" };
            fixture.dir.Write("resume.bin.part", fixture.content.substr(0, 1234));
            auto ec{ co_await downloader.AsyncDownload(fixture.Url("/file"), target) };
            check(!ec && read_file(target) == fixture.content, "resume must preserve existing bytes and append the remainder");
            check(fixture.log.Entries().front().starts_with("/file|bytes=1234-|"), "resume must send the on-disk offset");

            fixture.dir.Write("resume.bin.part", "stale prefix");
            ec = co_await downloader.AsyncDownload(fixture.Url("/ignore-range"), target);
            check(!ec && read_file(target) == fixture.content, "a 200 response to Range must restart the file");
            fixture.dir.Write("resume.bin.part", fixture.content + "extra");
            ec = co_await downloader.AsyncDownload(fixture.Url("/file"), target);
            check(!ec && read_file(target) == fixture.content, "an unsatisfiable stale part must restart the download");

            ec = co_await downloader.AsyncDownload(fixture.Url("/flaky"), target);
            check(!ec && read_file(target) == fixture.content, "an interrupted body must resume on retry");
            check(fixture.log.Count("/flaky") == 2, "one interrupted body must need one resumed attempt");

            auto config{ downloader.GetConfig() };
            config.resume = false;
            downloader.SetConfig(config);
            fixture.log.Clear();
            fixture.dir.Write("resume.bin.part", "discard me");
            ec = co_await downloader.AsyncDownload(fixture.Url("/file"), target);
            check(!ec && read_file(target) == fixture.content && fixture.log.Entries().front() == "/file||", "resume=false must discard the old part and omit Range");
        });
    }

    /**
     * @brief Check terminal errors, retry budgets, and rejection of malformed partial responses.
     */
    auto test_failures() -> void {
        Fixture fixture;
        with_downloader([&](net::io_context&, Downloader& downloader) -> net::awaitable<void> {
            auto const target{ fixture.dir.Write("preserved.txt", "old data") };
            auto ec{ co_await downloader.AsyncDownload(fixture.Url("/missing"), target) };
            check(ec == mhttp::Error::HttpStatus && downloader.HttpStatus() == Status::not_found && fixture.log.Count("/missing") == 1, "404 must fail immediately and retain its status");
            check(read_file(target) == "old data" && downloader.CurrentState() == Downloader::State::Failed, "failure must preserve the destination and report Failed");
            ec = co_await downloader.AsyncDownload(fixture.Url("/busy"), target);
            check(ec == mhttp::Error::HttpStatus && fixture.log.Count("/busy") == 3, "503 must use the configured two extra attempts");
            ec = co_await downloader.AsyncDownload(fixture.Url("/loop"), target);
            check(ec == mhttp::Error::TooManyRedirects && fixture.log.Count("/loop") == 6, "redirect loops must stop at the configured budget");
            ec = co_await downloader.AsyncDownload("file:///tmp/file", target);
            check(ec == mhttp::Error::InvalidUrl, "non-HTTP URLs must fail before acquiring a connection");
            ec = co_await downloader.AsyncDownload(fixture.Url("/bad-range"), target);
            check(ec == mhttp::Error::ProtocolViolation && read_file(target) == "old data", "a 206 without Content-Range must never publish corrupt data");
        });
    }

    /**
     * @brief Exercise fresh hits, revalidation, request directives, and representation isolation.
     */
    auto test_http_cache() -> void {
        Fixture fixture;
        auto cache{ std::make_shared<mhttp::client::DiskCache>(fixture.dir.path / "cache") };
        with_downloader([&](net::io_context&, Downloader& downloader) -> net::awaitable<void> {
            downloader.SetCache(cache);
            auto const target{ fixture.dir.path / "cached.bin" };
            for (int i{ 0 }; i < 2; ++i) {
                auto const ec{ co_await downloader.AsyncDownload(fixture.Url("/fresh"), target) };
                check(!ec && read_file(target) == fixture.content, "fresh cache hits must copy the full body");
            }
            check(fixture.log.Count("/fresh") == 1, "a fresh hit must avoid the network");
            mhttp::Headers revalidate;
            revalidate.Set(Field::cache_control, "no-cache");
            auto ec{ co_await downloader.AsyncDownload(fixture.Url("/fresh"), target, revalidate) };
            check(!ec && fixture.log.Count("/fresh") == 2, "request no-cache must force revalidation");

            for (int i{ 0 }; i < 3; ++i) {
                ec = co_await downloader.AsyncDownload(fixture.Url("/revalidate"), target);
                check(!ec && read_file(target) == fixture.content, "304 must recover the complete cached representation");
            }
            check(fixture.log.Count("/revalidate") == 2, "304 freshness metadata must prevent another round trip");
            auto const entries{ fixture.log.Entries() };
            check(std::ranges::find(entries, "/revalidate||\"v1\"") != entries.end(), "revalidation must send the stored ETag");

            for (auto const variant : { "one", "two" }) {
                mhttp::Headers headers;
                headers.Set("X-Variant", variant);
                ec = co_await downloader.AsyncDownload(fixture.Url("/vary"), target, headers);
                check(!ec && read_file(target) == variant, "Vary responses must not reuse another request's representation");
            }
            for (auto const credential : { "Bearer a", "Bearer b" }) {
                mhttp::Headers headers;
                headers.Set(Field::authorization, credential);
                ec = co_await downloader.AsyncDownload(fixture.Url("/fresh"), target, headers);
                check(!ec, "credential-scoped cache downloads must succeed");
            }
            check(fixture.log.Count("/fresh") == 4, "distinct credentials must have distinct cache entries");
        });
    }

    /**
     * @brief Pause a transfer, reject a second run, cancel, and reuse the downloader.
     */
    auto test_pause_cancel_and_reuse() -> void {
        Fixture fixture;
        with_downloader([&](net::io_context& ioc, Downloader& downloader) -> net::awaitable<void> {
            auto const target{ fixture.dir.path / "slow.bin" };
            std::optional<boost::system::error_code> result;
            std::uint64_t downloaded{ 0 };
            downloader.SetProgressCallback([&](Downloader::ProgressInfo const& info) {
                downloaded = info.downloaded_bytes;
                if (downloaded >= 1000) {
                    downloader.Pause();
                }
            });
            net::co_spawn(ioc, [&]() -> net::awaitable<void> { result = co_await downloader.AsyncDownload(fixture.Url("/slow"), target); }, net::detached);
            for (int i{ 0 }; i < 200 && downloader.CurrentState() != Downloader::State::Paused && !result; ++i) {
                co_await sleep_for(std::chrono::milliseconds{ 5 });
            }
            check(downloader.IsPaused() && downloader.CurrentState() == Downloader::State::Paused, "Pause must suspend the transfer");
            auto const paused_bytes{ downloaded };
            co_await sleep_for(std::chrono::milliseconds{ 30 });
            check(downloaded == paused_bytes && !result, "paused transfers must not report more downloaded bytes");
            auto const busy{ co_await downloader.AsyncDownload(fixture.Url("/file"), target) };
            check(busy == mhttp::Error::AlreadyInUse, "a second run must be rejected without changing the active transfer");
            downloader.Cancel();
            for (int i{ 0 }; i < 200 && !result; ++i) {
                co_await sleep_for(std::chrono::milliseconds{ 5 });
            }
            check(result && *result == net::error::operation_aborted && downloader.CurrentState() == Downloader::State::Cancelled, "Cancel must wake a paused transfer and report Cancelled");
            check(!std::filesystem::exists(target) && std::filesystem::file_size(target.string() + ".part") == paused_bytes, "cancellation must keep the resumable part and leave the destination absent");
            downloader.SetProgressCallback({});
            downloader.Resume();
            auto const ec{ co_await downloader.AsyncDownload(fixture.Url("/slow"), target) };
            check(!ec && read_file(target) == std::string(20000, 's'), "the downloader must remain reusable after cancellation");
        });
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("download and redirect", test_download_and_redirect);
    run_case("resume and retry", test_resume_and_retry);
    run_case("terminal failures", test_failures);
    run_case("HTTP cache", test_http_cache);
    run_case("pause cancel and reuse", test_pause_cancel_and_reuse);
    return finish("test_downloader");
}
