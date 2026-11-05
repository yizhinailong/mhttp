/**
 * @file test_client.cpp
 * @brief Exercise mhttp::client against a real mhttp server over loopback TCP.
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
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import nlohmann.json;
import mhttp;

#include "support.hpp"

using namespace test_support;
using mhttp::Field;
using mhttp::Json;
using mhttp::Status;
using mhttp::Verb;
using mhttp::client::BodyMode;
using mhttp::client::HttpClient;
using mhttp::client::HttpClientPool;
using mhttp::client::LazyRequest;

namespace {

    /**
     * @brief Run a client coroutine on a fresh io_context until it and its work finish.
     * @param body Coroutine factory receiving the context.
     */
    auto run_client(std::function<net::awaitable<void>(net::io_context&)> const& body) -> void {
        net::io_context ioc;
        bool finished{ false };
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true;
        }, net::detached);
        ioc.run();
        check(finished, "the client coroutine must run to completion");
    }

    auto sleep_for(std::chrono::milliseconds duration) -> net::awaitable<void> {
        net::steady_timer timer{ co_await net::this_coro::executor, duration };
        co_await timer.async_wait(net::use_awaitable);
    }

    /**
     * @brief Answer with a JSON description of the request.
     * @param req Request; the body is read as text.
     * @param resp Response.
     */
    auto echo_handler(mhttp::Request& req, mhttp::Response& resp) -> net::awaitable<void> {
        auto const body{ co_await req.ReadString() };
        resp.SetJsonContent(Json{
            { "method", std::string{ req.MethodString() } },
            { "target", std::string{ req.Target() } },
            { "body", body },
            { "port", req.RemoteEndpoint().port() },
            { "host", std::string{ req[Field::host] } },
            { "type", std::string{ req[Field::content_type] } },
            { "length", std::string{ req[Field::content_length] } },
            { "agent", std::string{ req[Field::user_agent] } },
            { "auth", std::string{ req[Field::authorization] } },
            { "cookie", std::string{ req[Field::cookie] } },
            { "keep", std::string{ req["X-Keep"] } },
        });
    }

    auto register_echo(mhttp::HttpServer& server, std::string_view path) -> void {
        server.GetRouter().SetLazyHttpHandler<Verb::get, Verb::post, Verb::put, Verb::patch, Verb::delete_, Verb::options, Verb::head>(std::string{ path }, echo_handler);
    }

    /**
     * @brief Return the echoed request description of a response.
     * @param result Send result.
     * @return Echo document, or an empty object when the send failed or was not JSON.
     */
    auto echo_of(mhttp::Result<mhttp::client::Response> const& result) -> Json {
        if (!result || result->Type() != mhttp::BodyType::Json) {
            return Json::object();
        }
        return result->AsJson();
    }

    auto port_of(mhttp::Result<mhttp::client::Response> const& result) -> int {
        return echo_of(result).value("port", -1);
    }

    auto test_methods_and_keep_alive() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) { register_echo(server, "/echo"); } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", srv.Endpoint().port() };
            mhttp::QueryParams params;
            params.Add("q", std::string_view{ "1 2" });
            mhttp::Headers headers;
            headers.Set("X-Keep", "yes");
            auto const get{ co_await client.AsyncGet("/echo", params, headers) };
            auto const echo = echo_of(get);
            check(get && get->Result() == Status::ok, "GET must succeed");
            check(echo.value("method", "") == "GET" && echo.value("target", "") == "/echo?q=1+2", std::format("query parameters must be appended, got {}", echo.dump()));
            check(echo.value("host", "") == std::format("127.0.0.1:{}", srv.Endpoint().port()), "Host must name host and non-default port");
            check(!echo.value("agent", "").empty() && echo.value("keep", "") == "yes", "User-Agent and caller headers must be sent");
            check(echo.value("length", "x").empty(), "a body-less GET must not announce Content-Length");
            auto const first_port{ port_of(get) };

            auto const post{ co_await client.AsyncPost("/echo", "hello", "text/plain") };
            check(echo_of(post).value("body", "") == "hello" && echo_of(post).value("type", "") == "text/plain", "string bodies must be sent with their type");
            auto const json{ co_await client.AsyncPut("/echo", Json{ { "a", 1 } }) };
            check(echo_of(json).value("method", "") == "PUT" && echo_of(json).value("body", "") == R"({"a":1})", "JSON bodies must be serialized");
            check(echo_of(json).value("type", "").starts_with("application/json"), "JSON bodies must be typed");
            auto const empty_post{ co_await client.AsyncPost("/echo") };
            check(echo_of(empty_post).value("length", "") == "0", "an empty POST must announce Content-Length: 0");
            check(echo_of(co_await client.AsyncPatch("/echo")).value("method", "") == "PATCH", "PATCH must be sent");
            check(echo_of(co_await client.AsyncDelete("/echo")).value("method", "") == "DELETE", "DELETE must be sent");
            check(echo_of(co_await client.AsyncOptions("/echo")).value("method", "") == "OPTIONS", "OPTIONS must be sent");

            auto const head{ co_await client.AsyncHead("/echo") };
            check(head && head->Result() == Status::ok && head->ContentLength().value_or(0) > 0 && head->AsString().empty(), "HEAD must report a length without a body");
            auto const after{ co_await client.AsyncGet("/echo") };
            check(port_of(after) == first_port, "every request must reuse the keep-alive connection");
            check(!client.HasActiveSession(), "eager requests must leave the client idle");
        });
    }

    auto test_lazy_bodies_and_single_flight() -> void {
        std::string const big(100 * 1024, 'b');
        TestServer srv{ [&](mhttp::HttpServer& server) {
            register_echo(server, "/echo");
            server.GetRouter().SetHttpHandler(Verb::get, "/big", [&](mhttp::Request&, mhttp::Response& resp) { resp.SetStringContent(big, "application/octet-stream"); });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", srv.Endpoint().port() };
            auto const first_port{ port_of(co_await client.AsyncGet("/echo")) };

            auto lazy{ co_await client.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/big" }, BodyMode::Lazy) };
            check(lazy && !lazy->IsBodyDone() && client.HasActiveSession(), "a lazy response must leave its body unread and the client busy");
            auto const busy{ co_await client.AsyncGet("/echo") };
            check(!busy && busy.error() == mhttp::Error::AlreadyInUse, "a request while a body is unread must fail with AlreadyInUse");
            std::size_t total{ 0 };
            std::array<char, 4096> buffer{};
            for (;;) {
                boost::system::error_code ec;
                auto const n{ co_await lazy->ReadSomeRaw(net::buffer(buffer), ec) };
                if (ec || n == 0) {
                    check(!ec, std::format("raw reads must not fail: {}", ec.message()));
                    break;
                }
                total += n;
            }
            check(total == big.size() && lazy->IsBodyDone(), "raw reads must deliver the whole body");
            check(!client.HasActiveSession(), "a fully read body must free the client");
            check(port_of(co_await client.AsyncGet("/echo")) == first_port, "the connection must be reused after a fully read body");

            auto text{ co_await client.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/big" }, BodyMode::Lazy) };
            auto const read{ text ? co_await text->ReadString() : mhttp::Result<std::string>{} };
            check(read && *read == big, "ReadString must materialize a lazy body");

            {
                auto abandoned{ co_await client.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/big" }, BodyMode::Lazy) };
                check(abandoned.has_value(), "the lazy request must succeed");
            }
            check(!client.HasActiveSession(), "destroying an unread response must free the client");
            auto const reconnected{ co_await client.AsyncGet("/echo") };
            check(reconnected && port_of(reconnected) != first_port, "an abandoned body must close the connection, not be parsed as the next response");
        });
    }

    auto test_redirects() -> void {
        TestServer other{ [](mhttp::HttpServer& server) { register_echo(server, "/landing"); } };
        auto const other_port{ other.Endpoint().port() };
        TestServer srv{ [&](mhttp::HttpServer& server) {
            register_echo(server, "/r/target");
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::get, "/r/relative", [](mhttp::Request&, mhttp::Response& resp) { resp.SetRedirect("target?from=relative", Status::found); });
            router.SetHttpHandler(Verb::get, "/r/absolute", [](mhttp::Request& req, mhttp::Response& resp) { resp.SetRedirect(std::format("http://{}/r/target?from=absolute", req[Field::host]), Status::moved_permanently); });
            router.SetHttpHandler(Verb::get, "/r/network", [](mhttp::Request& req, mhttp::Response& resp) { resp.SetRedirect(std::format("//{}/r/target?from=network", req[Field::host]), Status::found); });
            router.SetHttpHandler(Verb::post, "/r/see-other", [](mhttp::Request&, mhttp::Response& resp) { resp.SetRedirect("/r/target?from=303", Status::see_other); });
            router.SetHttpHandler(Verb::post, "/r/temporary", [](mhttp::Request&, mhttp::Response& resp) { resp.SetRedirect("/r/target?from=307", Status::temporary_redirect); });
            router.SetHttpHandler(Verb::get, "/r/loop", [](mhttp::Request&, mhttp::Response& resp) { resp.SetRedirect("/r/loop", Status::found); });
            router.SetHttpHandler(Verb::get, "/r/cross", [other_port](mhttp::Request&, mhttp::Response& resp) { resp.SetRedirect(std::format("http://127.0.0.1:{}/landing", other_port), Status::found); });
            router.SetHttpHandler(Verb::get, "/r/bad", [](mhttp::Request&, mhttp::Response& resp) { resp.SetRedirect("ftp://example.com/x", Status::found); });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", srv.Endpoint().port() };
            auto const unfollowed{ co_await client.AsyncGet("/r/relative") };
            check(unfollowed && unfollowed->Result() == Status::found && (*unfollowed)[Field::location] == "target?from=relative", "without a redirect limit, 3xx must be returned as is");

            client.SetMaxRedirects(5);
            auto const relative{ co_await client.AsyncGet("/r/relative") };
            check(echo_of(relative).value("target", "") == "/r/target?from=relative", "relative Location must resolve against the request target");
            auto const port{ port_of(relative) };
            auto const absolute{ co_await client.AsyncGet("/r/absolute") };
            check(echo_of(absolute).value("target", "") == "/r/target?from=absolute" && port_of(absolute) == port, "a same-origin absolute Location must reuse the connection");
            auto const network{ co_await client.AsyncGet("/r/network") };
            check(echo_of(network).value("target", "") == "/r/target?from=network", "a network-path Location must inherit the scheme");

            mhttp::client::Request see_other{ Verb::post, "/r/see-other" };
            see_other.SetStringBody("payload", "text/plain");
            auto const converted = echo_of(co_await client.AsyncSendRequest(see_other));
            check(converted.value("method", "") == "GET" && converted.value("body", "x").empty() && converted.value("type", "x").empty(), "303 must turn POST into a body-less GET");

            mhttp::client::Request temporary{ Verb::post, "/r/temporary" };
            temporary.SetStringBody("payload", "text/plain");
            auto const kept = echo_of(co_await client.AsyncSendRequest(temporary));
            // httplib re-sent the old Content-Length with an exhausted body source here.
            check(kept.value("method", "") == "POST" && kept.value("body", "") == "payload", std::format("307 must replay method and body, got {}", kept.dump()));

            auto const loop{ co_await client.AsyncGet("/r/loop") };
            check(!loop && loop.error() == mhttp::Error::TooManyRedirects, "a redirect loop must end with TooManyRedirects");
            auto const bad{ co_await client.AsyncGet("/r/bad") };
            check(!bad && bad.error() == mhttp::Error::InvalidUrl, "a non-HTTP Location must fail with InvalidUrl");

            mhttp::Headers secrets;
            secrets.Set(Field::authorization, "Bearer secret");
            secrets.Set(Field::cookie, "c=1");
            secrets.Set("X-Keep", "yes");
            auto const cross = echo_of(co_await client.AsyncGet("/r/cross", {}, secrets));
            check(cross.value("target", "") == "/landing" && cross.value("keep", "") == "yes", std::format("a cross-origin redirect must reach the other server, got {}", cross.dump()));
            check(cross.value("auth", "x").empty() && cross.value("cookie", "x").empty(), "credentials must not follow a redirect to another origin");
            check(cross.value("host", "") == std::format("127.0.0.1:{}", other_port), "Host must name the new origin (httplib kept the old one)");
        });
    }

    auto test_stale_connection_retry() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            server.SetReadTimeout(std::chrono::milliseconds{ 150 });
            register_echo(server, "/echo");
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", srv.Endpoint().port() };
            auto const first{ co_await client.AsyncGet("/echo") };
            check(first.has_value(), "the first request must succeed");
            co_await sleep_for(std::chrono::milliseconds{ 500 });
            auto const second{ co_await client.AsyncPost("/echo", "again", "text/plain") };
            check(second && echo_of(second).value("body", "") == "again", std::format("a request on an idle connection the server closed must be retried, got {}", second ? "a response" : second.error().message()));
            check(port_of(second) != port_of(first), "the retry must use a new connection");
        });
    }

    auto test_timeout_and_errors() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            register_echo(server, "/echo");
            server.GetRouter().SetHttpHandler(Verb::get, "/slow", [](mhttp::Request&, mhttp::Response& resp) -> net::awaitable<void> {
                net::steady_timer timer{ co_await net::this_coro::executor, std::chrono::milliseconds{ 400 } };
                co_await timer.async_wait(net::use_awaitable);
                resp.SetStringContent("late", "text/plain");
            });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", srv.Endpoint().port() };
            client.SetTimeout(std::chrono::milliseconds{ 100 });
            auto const slow{ co_await client.AsyncGet("/slow") };
            check(!slow && slow.error() == boost::beast::error::timeout, std::format("a slow response must time out, got {}", slow ? "a response" : slow.error().message()));
            client.SetTimeout(std::chrono::seconds{ 5 });
            check((co_await client.AsyncGet("/echo")).has_value(), "the client must recover after a timeout");

            client.SetTimeoutPolicy(mhttp::client::TimeoutPolicy::Step);
            mhttp::client::Request expect{ Verb::post, "/echo" };
            expect.Set(Field::expect, "100-continue");
            expect.SetStringBody("continued", "text/plain");
            auto const continued{ co_await client.AsyncSendRequest(expect) };
            check(continued && continued->Result() == Status::ok && echo_of(continued).value("body", "") == "continued", "an interim 100 Continue must be skipped (httplib returned it)");

            bool threw{ false };
            try {
                HttpClient invalid{ ioc, "ftp://example.com" };
            } catch (boost::system::system_error const& e) {
                threw = e.code() == mhttp::Error::InvalidUrl;
            }
            check(threw, "a non-HTTP URL must be rejected by the constructor");
            HttpClient tls{ ioc, "https://127.0.0.1:1" };
            auto const secure{ co_await tls.AsyncGet("/") };
            check(!secure && secure.error() == net::error::operation_not_supported, "HTTPS must fail clearly until TLS is ported");

            HttpClient refused{ ioc, "127.0.0.1", 1 };
            auto const unreachable{ co_await refused.AsyncGet("/") };
            check(!unreachable && !refused.HasActiveSession(), "a failed connect must report an error and leave the client idle");
        });
    }

    auto test_lazy_request() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            register_echo(server, "/echo");
            server.GetRouter().SetLazyHttpHandler(Verb::post, "/upload", [](mhttp::Request& req, mhttp::Response& resp) -> net::awaitable<void> {
                std::array<char, 5> buffer{};
                std::size_t total{ 0 };
                for (;;) {
                    auto const n{ co_await req.ReadSomeRaw(net::buffer(buffer)) };
                    if (n == 0) {
                        break;
                    }
                    total += n;
                }
                resp.SetStringContent(std::format("{} {}", total, req[Field::transfer_encoding]), "text/plain");
            });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", srv.Endpoint().port() };
            auto writer{ client.CreateLazyRequest() };
            check(client.HasActiveSession(), "a live writer must keep the client busy");
            boost::system::error_code ec;
            co_await writer->WriteHeader(Verb::post, "/upload", mhttp::Headers{}, LazyRequest::Mode::Chunked, ec);
            check(!ec, std::format("WriteHeader must succeed: {}", ec.message()));

            auto const busy{ co_await client.AsyncGet("/echo") };
            check(!busy && busy.error() == mhttp::Error::AlreadyInUse, "a request during a streamed upload must fail with AlreadyInUse");
            auto other{ client.CreateLazyRequest() };
            boost::system::error_code other_ec;
            co_await other->WriteHeader(Verb::post, "/upload", mhttp::Headers{}, LazyRequest::Mode::Chunked, other_ec);
            check(other_ec == mhttp::Error::AlreadyInUse, "a second writer must not interleave with the first");
            other.reset();

            for (std::string_view const part : { "hello", " ", "world!" }) {
                co_await writer->WriteBody(net::buffer(part), true, ec);
            }
            co_await writer->WriteBody(net::const_buffer{}, false, ec);
            auto const chunked{ co_await writer->ReadResponse() };
            check(chunked && chunked->AsString() == "12 chunked", std::format("a chunked upload must arrive whole, got '{}'", chunked ? chunked->AsString() : chunked.error().message()));

            mhttp::Headers relay_headers;
            relay_headers.Set(Field::content_length, "10");
            co_await writer->WriteHeader(Verb::post, "/upload", relay_headers, LazyRequest::Mode::Relay, ec);
            co_await writer->WriteBody(net::buffer(std::string_view{ "01234" }), true, ec);
            co_await writer->WriteBody(net::buffer(std::string_view{ "56789" }), false, ec);
            auto const relay{ co_await writer->ReadResponse() };
            check(relay && relay->AsString() == "10 ", "relay mode must keep the caller's Content-Length framing");

            writer.reset();
            check(!client.HasActiveSession(), "releasing the writer must free the client");
            check((co_await client.AsyncGet("/echo")).has_value(), "the connection must serve regular requests afterwards");
        });
    }

    auto test_stream_readers() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::get, "/events", [](mhttp::Request&, mhttp::Response& resp) -> net::awaitable<void> {
                auto sse{ resp.CreateSseWriter() };
                co_await sse->Begin();
                co_await sse->SendRetry(std::chrono::milliseconds{ 1500 }, true);
                co_await sse->SendEvent("line1\nline2", "update", "7", true);
                co_await sse->SendComment("ping", true);
                co_await sse->SendEvent("bye", "", "", false);
            });
            router.SetHttpHandler(Verb::get, "/ndjson", [](mhttp::Request&, mhttp::Response& resp) -> net::awaitable<void> {
                auto writer{ resp.CreateNdjsonWriter() };
                co_await writer->Begin();
                co_await writer->Write(Json{ { "n", 1 } }, true);
                co_await writer->Write(Json{}, false);
            });
            router.SetHttpHandler(Verb::get, "/ndjson-raw", [](mhttp::Request& req, mhttp::Response& resp) -> net::awaitable<void> {
                auto writer{ resp.CreateStreamWriter() };
                co_await writer->WriteHeader(Status::ok, mhttp::Headers{}, mhttp::StreamWriter::Mode::Chunked);
                auto const text{ req.Query().Has("bad") ? std::string{ "{oops}\n" } : std::string{ "{\"a\":1}\r\n\n{\"b\":2}" } };
                co_await writer->WriteBody(net::buffer(text), false);
            });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", srv.Endpoint().port() };
            auto events{ co_await client.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/events" }, BodyMode::Lazy) };
            check(events.has_value(), "the SSE request must succeed");
            if (events) {
                auto reader{ events->CreateSseReader() };
                std::vector<mhttp::client::SseEvent> received;
                for (;;) {
                    auto const event{ co_await reader->ReadEvent() };
                    if (!event || !*event) {
                        check(event.has_value(), "reading events must not fail");
                        break;
                    }
                    received.push_back(**event);
                }
                check(received.size() == 3 && reader->IsDone(), std::format("three events must arrive, got {}", received.size()));
                if (received.size() == 3) {
                    check(received[0].retry == std::chrono::milliseconds{ 1500 }, "the retry field must be parsed");
                    check(received[1].id == "7" && received[1].event == "update" && received[1].data == "line1\nline2", "multi-line data must be joined");
                    check(received[2].data == "bye" && received[2].event.empty(), "the final event must arrive");
                }
            }

            auto ndjson{ co_await client.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/ndjson" }, BodyMode::Lazy) };
            if (ndjson) {
                auto reader{ ndjson->CreateNdjsonReader() };
                auto const first{ co_await reader->Read() };
                auto const second{ co_await reader->Read() };
                auto const end{ co_await reader->Read() };
                check(first && *first && (*first)->value("n", 0) == 1, "the first document must be read");
                check(second && *second && (*second)->is_null(), "a \"null\" line must be a document, not the end (httplib could not tell)");
                check(end && !*end && reader->IsDone(), "the end of the stream must be std::nullopt");
            }

            auto raw{ co_await client.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/ndjson-raw" }, BodyMode::Lazy) };
            if (raw) {
                auto reader{ raw->CreateNdjsonReader() };
                auto const a{ co_await reader->Read() };
                auto const b{ co_await reader->Read() };
                check(a && *a && (*a)->value("a", 0) == 1, "CRLF lines must parse and blank lines be skipped");
                check(b && *b && (*b)->value("b", 0) == 2, "a final line without '\\n' must still be a record");
            }
            auto bad{ co_await client.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/ndjson-raw?bad=1" }, BodyMode::Lazy) };
            if (bad) {
                auto const parsed{ co_await bad->CreateNdjsonReader()->Read() };
                check(!parsed && parsed.error() == mhttp::Error::InvalidJson, "a malformed line must fail with InvalidJson");
            }
        });
    }

    auto test_bodies_and_download() -> void {
        TempDir dir;
        auto const attachment{ dir.Write("note.txt", "from a file") };
        std::string const large(50 * 1000, 'd');
        TestServer srv{ [&](mhttp::HttpServer& server) {
            register_echo(server, "/echo");
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::post, "/form", [](mhttp::Request& req, mhttp::Response& resp) {
                auto const& form{ req.AsFormData() };
                auto const file{ form.FieldByName("file") };
                resp.SetStringContent(std::format("{}|{}|{}", form.Content("a").value_or("?"), file ? file->filename : "?", form.Content("file").value_or("?")), "text/plain");
            });
            router.SetHttpHandler(Verb::get, "/large", [&](mhttp::Request&, mhttp::Response& resp) { resp.SetStringContent(large, "application/octet-stream"); });
            router.SetHttpHandler(Verb::get, "/chunks", [](mhttp::Request&, mhttp::Response& resp) -> net::awaitable<void> {
                auto writer{ resp.CreateStreamWriter() };
                co_await writer->WriteHeader(Status::ok, mhttp::Headers{}, mhttp::StreamWriter::Mode::Chunked);
                std::string const chunk(1000, 'c');
                for (int i{ 0 }; i < 10; ++i) {
                    co_await writer->WriteBody(net::buffer(chunk), i < 9);
                }
            });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", srv.Endpoint().port() };
            mhttp::client::Request form{ Verb::post, "/form" };
            std::vector<mhttp::FormData::Field> fields;
            fields.push_back(mhttp::FormData::Field{ .name = "a", .content = "alpha" });
            fields.push_back(mhttp::FormData::Field{ .name = "file", .filename = "note.txt", .content_type = "text/plain", .file_path = attachment });
            form.SetFormDataBody(std::move(fields));
            auto const uploaded{ co_await client.AsyncSendRequest(form) };
            check(uploaded && uploaded->AsString() == "alpha|note.txt|from a file", std::format("multipart bodies must stream file parts, got '{}'", uploaded ? uploaded->AsString() : uploaded.error().message()));

            mhttp::client::Request file{ Verb::put, "/echo" };
            check(file.SetFileBody(attachment).has_value(), "an existing file must be accepted");
            auto const sent = echo_of(co_await client.AsyncSendRequest(file));
            check(sent.value("body", "") == "from a file" && sent.value("type", "") == "text/plain", "file bodies must be typed by extension");
            check(!file.SetFileBody(dir.path / "missing.txt").has_value(), "a missing file must be reported");

            auto const target{ dir.path / "download.bin" };
            auto const downloaded{ co_await client.AsyncDownload(Verb::get, "/large", target) };
            std::ifstream in{ target, std::ios::binary };
            std::string const content{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
            in.close();
            check(downloaded && content == large, "a download must write the whole body to the file");

            client.SetBodyLimit(3000);
            auto const partial{ dir.path / "partial.bin" };
            auto const failed{ co_await client.AsyncDownload(Verb::get, "/chunks", partial) };
            check(!failed && failed.error() == boost::beast::http::error::body_limit, "a body over the limit must fail the download");
            check(!std::filesystem::exists(partial), "a failed download must not leave a partial file (httplib kept it)");
        });
    }

    auto test_pool() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) { register_echo(server, "/echo"); } };
        auto const port{ srv.Endpoint().port() };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClientPool pool{ ioc.get_executor(), mhttp::client::PoolParams{ .max_size = 1 } };
            auto first{ co_await pool.AsyncAcquire("127.0.0.1", port) };
            check(static_cast<bool>(first), "the first acquire must create a connection");
            auto const first_port{ first ? port_of(co_await first->AsyncGet("/echo")) : -1 };

            auto const fail_fast{ co_await pool.AsyncAcquire("127.0.0.1", port, mhttp::url::Scheme::Plain, std::chrono::seconds{ 0 }) };
            check(!fail_fast && fail_fast.GetError() == mhttp::Error::AcquireTimeout, "a full pool must fail fast with a zero wait");

            std::optional<int> waiter_port;
            net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                auto handle{ co_await pool.AsyncAcquire(std::format("http://127.0.0.1:{}/ignored", port), std::chrono::seconds{ 5 }) };
                waiter_port = handle ? port_of(co_await handle->AsyncGet("/echo")) : -2;
            }, net::detached);
            co_await sleep_for(std::chrono::milliseconds{ 50 });
            check(!waiter_port.has_value(), "a waiter must block while the only connection is lent");
            first.Release();
            for (int i{ 0 }; i < 200 && !waiter_port; ++i) {
                co_await sleep_for(std::chrono::milliseconds{ 10 });
            }
            check(waiter_port == first_port, "a released connection must go to the waiter and be reused");
            check(pool.TotalCount() == 1, std::format("the pool must not open a second connection, total={}", pool.TotalCount()));
            auto const stats{ co_await pool.AsyncStats("127.0.0.1", port) };
            check(stats.idle == 1 && stats.active == 0, std::format("the connection must be idle again, idle={} active={}", stats.idle, stats.active));

            {
                auto busy{ co_await pool.AsyncAcquire("127.0.0.1", port) };
                check(static_cast<bool>(busy), "the idle connection must be lent again");
                if (busy) {
                    auto const lazy{ co_await busy->AsyncSendRequest(mhttp::client::Request{ Verb::get, "/echo" }, BodyMode::Lazy) };
                    check(lazy.has_value(), "a lazy request on a pooled client must succeed");
                    busy.Release();
                    co_await sleep_for(std::chrono::milliseconds{ 50 });
                    check(pool.TotalCount() == 0, "a connection returned with an unread body must be dropped, not pooled");
                }
            }

            auto const invalid{ co_await pool.AsyncAcquire("not a url") };
            check(!invalid && invalid.GetError() == mhttp::Error::InvalidUrl, "an invalid URL must fail with InvalidUrl");
            pool.Stop();
            auto const stopped{ co_await pool.AsyncAcquire("127.0.0.1", port) };
            check(!stopped && stopped.GetError() == boost::system::errc::operation_canceled, "a stopped pool must refuse to lend");
        });
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("methods and keep-alive", test_methods_and_keep_alive);
    run_case("lazy bodies and single flight", test_lazy_bodies_and_single_flight);
    run_case("redirects", test_redirects);
    run_case("stale connection retry", test_stale_connection_retry);
    run_case("timeouts and errors", test_timeout_and_errors);
    run_case("lazy request", test_lazy_request);
    run_case("stream readers", test_stream_readers);
    run_case("bodies and download", test_bodies_and_download);
    run_case("connection pool", test_pool);
    return finish("test_client");
}
