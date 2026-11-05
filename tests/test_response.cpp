/**
 * @file test_response.cpp
 * @brief Verify response content setters, file serving with ranges, and the streaming writers.
 */
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import nlohmann.json;
import mhttp;

#include "support.hpp"

using namespace test_support;
using mhttp::Request;
using mhttp::Response;
using mhttp::Verb;

namespace {

    auto test_content_setters() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::get, "/text", [](Request&, Response& resp) { resp.SetStringContent("hi", "text/plain", mhttp::Status::created); });
            router.SetHttpHandler(Verb::get, "/json", [](Request&, Response& resp) { resp.SetJsonContent(mhttp::Json{ { "ok", true } }); });
            router.SetHttpHandler(Verb::get, "/redirect", [](Request&, Response& resp) { resp.SetRedirect("/text", mhttp::Status::found); });
            router.SetHttpHandler(Verb::get, "/error", [](Request&, Response& resp) { resp.SetErrorContent(mhttp::Status::service_unavailable); });
            router.SetHttpHandler(Verb::get, "/none", [](Request&, Response& resp) { resp.SetEmptyContent(mhttp::Status::no_content); });
            router.SetHttpHandler(Verb::get, "/headers", [](Request&, Response& resp) {
                resp.Set("X-One", "1");
                resp.Insert("Set-Cookie", "a=1");
                resp.Insert("Set-Cookie", "b=2");
                resp.SetStringContent(std::format("{} {}", resp.Count("Set-Cookie"), resp.Has("X-One")), "text/plain");
            });
            router.SetHttpHandler(Verb::get, "/nothing", [](Request&, Response&) {});
        } };
        Client client{ srv.Endpoint() };
        auto const text{ client.Get("/text") };
        check(text && text->result() == http::status::created && text->body() == "hi", "SetStringContent must set status and body");
        auto const json{ client.Get("/json") };
        check(json && nlohmann::json::parse(json->body()) == nlohmann::json{ { "ok", true } } && (*json)[http::field::cache_control] == "no-store", "SetJsonContent must serialize and mark no-store");
        auto const redirect{ client.Get("/redirect") };
        check(redirect && redirect->result() == http::status::found && (*redirect)[http::field::location] == "/text" && redirect->body().empty(), "SetRedirect must set Location with an empty body");
        auto const error{ client.Get("/error") };
        check(error && error->result() == http::status::service_unavailable && error->body().find("503 Service Unavailable") != std::string::npos, "SetErrorContent must render the HTML page");
        auto const none{ client.Get("/none") };
        check(none && none->result() == http::status::no_content && !none->has_content_length(), "204 must not carry Content-Length (RFC 9110)");
        auto const headers{ client.Get("/headers") };
        check(headers && headers->body() == "2 true" && headers->count(http::field::set_cookie) == 2, "Insert must keep duplicate fields");
        auto const nothing{ client.Get("/nothing") };
        check(nothing && nothing->result() == http::status::not_found, "a response left untouched stays 404, as in httplib");
        check(client.Get("/text").has_value(), "the connection must stay usable after all of the above");
    }

    auto test_form_data_response() -> void {
        TempDir dir;
        auto const file{ dir.Write("attach.txt", "from disk") };
        TestServer srv{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/form", [&](Request&, Response& resp) {
                std::vector<mhttp::FormData::Field> fields;
                fields.push_back(mhttp::FormData::Field{ .name = "a", .content = "alpha" });
                fields.push_back(mhttp::FormData::Field{ .name = "f", .filename = "attach.txt", .content_type = "text/plain", .file_path = file });
                resp.SetFormDataContent(std::move(fields));
            });
        } };
        Client client{ srv.Endpoint() };
        auto const resp{ client.Get("/form") };
        check(resp && (*resp)[http::field::content_type].starts_with("multipart/form-data; boundary="), "multipart responses must announce their boundary");
        check(resp && resp->body().find("name=\"a\"\r\n\r\nalpha\r\n") != std::string::npos, "plain parts must be serialized");
        check(resp && resp->body().find("filename=\"attach.txt\"\r\nContent-Type: text/plain\r\n\r\nfrom disk\r\n") != std::string::npos, "file parts must stream from disk");
        check(std::filesystem::exists(file), "files referenced by a response must stay the caller's");
    }

    auto test_file_and_ranges() -> void {
        TempDir dir;
        std::string content;
        for (int i{ 0 }; i < 1000; ++i) {
            content += static_cast<char>('a' + i % 26);
        }
        auto const file{ dir.Write("data.txt", content) };
        TestServer srv{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler<Verb::get, Verb::head>("/file", [&](Request& req, Response& resp) { resp.SetFileContent(file, req.Base()); });
        } };
        Client client{ srv.Endpoint() };

        auto const full{ client.Get("/file") };
        check(full && full->result() == http::status::ok && full->body() == content, "the whole file must be served");
        check(full && (*full)[http::field::content_type] == "text/plain" && (*full)[http::field::accept_ranges] == "bytes", "type and Accept-Ranges must be set");
        auto const etag{ full ? std::string{ (*full)[http::field::etag] } : std::string{} };
        check(etag.starts_with("W/\"") && etag.ends_with("\""), "the ETag must be a quoted weak validator");

        auto const cached{ client.Get("/file", { { "If-None-Match", etag } }) };
        check(cached && cached->result() == http::status::not_modified && cached->body().empty(), "a matching If-None-Match must give 304");
        auto const unmodified{ client.Get("/file", { { "If-Modified-Since", full ? std::string{ (*full)[http::field::last_modified] } : "" } }) };
        check(unmodified && unmodified->result() == http::status::not_modified, "a matching If-Modified-Since must give 304");

        auto const single{ client.Get("/file", { { "Range", "bytes=10-19" } }) };
        check(single && single->result() == http::status::partial_content && single->body() == content.substr(10, 10), "a single range must be served exactly");
        check(single && (*single)[http::field::content_range] == "bytes 10-19/1000", "Content-Range must describe the range");
        auto const suffix{ client.Get("/file", { { "Range", "bytes=-5" } }) };
        check(suffix && suffix->body() == content.substr(995), "a suffix range must serve the last bytes");

        auto const multi{ client.Get("/file", { { "Range", "bytes=0-2,100-104" } }) };
        check(multi && multi->result() == http::status::partial_content, "multiple ranges must be 206");
        check(multi && (*multi)[http::field::content_type].starts_with("multipart/byteranges; boundary="), "multiple ranges must use multipart/byteranges");
        // httplib dropped the last byte of every part; both parts must be complete here.
        check(multi && multi->body().find("Content-Range: bytes 0-2/1000\r\n\r\n" + content.substr(0, 3) + "\r\n") != std::string::npos, "the first part must hold all three bytes");
        check(multi && multi->body().find("Content-Range: bytes 100-104/1000\r\n\r\n" + content.substr(100, 5) + "\r\n") != std::string::npos, "the second part must hold all five bytes");

        auto const bad{ client.Get("/file", { { "Range", "bytes=5000-6000" } }) };
        check(bad && bad->result() == http::status::range_not_satisfiable && (*bad)[http::field::content_range] == "bytes */1000", "an unsatisfiable range must be 416");

        auto const head{ client.Send(make_request(http::verb::head, "/file")) };
        check(head && head->body().empty() && (*head)[http::field::content_length] == "1000", "HEAD on a file must report its length without sending it");
    }

    auto test_stream_writer() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::get, "/chunked", [](Request&, Response& resp) -> net::awaitable<void> {
                auto writer{ resp.CreateStreamWriter() };
                mhttp::Headers headers;
                headers.Set(mhttp::Field::content_type, "text/plain");
                co_await writer->WriteHeader(mhttp::Status::ok, headers, mhttp::StreamWriter::Mode::Chunked);
                for (int i{ 0 }; i < 3; ++i) {
                    auto const part{ std::format("part{};", i) };
                    co_await writer->WriteBody(net::buffer(part), true);
                }
                co_await writer->WriteBody(net::const_buffer{}, false);
            });
            router.SetHttpHandler(Verb::get, "/relay", [](Request&, Response& resp) -> net::awaitable<void> {
                auto writer{ resp.CreateStreamWriter() };
                mhttp::Headers headers;
                headers.Set(mhttp::Field::content_length, "10");
                co_await writer->WriteHeader(mhttp::Status::accepted, headers, mhttp::StreamWriter::Mode::Relay);
                co_await writer->WriteBody(net::buffer(std::string_view{ "01234" }), true);
                co_await writer->WriteBody(net::buffer(std::string_view{ "56789" }), false);
            });
            router.SetHttpHandler(Verb::get, "/unfinished", [](Request&, Response& resp) -> net::awaitable<void> {
                auto writer{ resp.CreateStreamWriter() };
                co_await writer->WriteHeader(mhttp::Status::ok, mhttp::Headers{}, mhttp::StreamWriter::Mode::Chunked);
                co_await writer->WriteBody(net::buffer(std::string_view{ "partial" }), true);
            });
        } };
        Client client{ srv.Endpoint() };
        auto const chunked{ client.Get("/chunked") };
        check(chunked && chunked->chunked() && chunked->body() == "part0;part1;part2;", "chunked streaming must deliver every chunk");
        auto const relay{ client.Get("/relay") };
        check(relay && relay->result() == http::status::accepted && !relay->chunked() && relay->body() == "0123456789", "relay mode must keep the caller's Content-Length framing");
        check(relay && relay->keep_alive(), "a finished stream must keep the connection alive");
        auto const unfinished{ client.Get("/unfinished") };
        check(!unfinished.has_value(), "an unfinished stream must end with the connection closed, not a complete message");
    }

    auto test_sse_and_ndjson() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::get, "/events", [](Request&, Response& resp) -> net::awaitable<void> {
                auto sse{ resp.CreateSseWriter() };
                co_await sse->Begin();
                co_await sse->SendRetry(std::chrono::milliseconds{ 1500 }, true);
                co_await sse->SendEvent("line1\nline2", "update", "7", true);
                co_await sse->SendComment("ping", true);
                co_await sse->SendEvent("bye", "", "", false);
            });
            router.SetHttpHandler(Verb::get, "/ndjson", [](Request&, Response& resp) -> net::awaitable<void> {
                auto writer{ resp.CreateNdjsonWriter() };
                co_await writer->Begin();
                co_await writer->Write(mhttp::Json{ { "n", 1 } }, true);
                co_await writer->Write(mhttp::Json{ { "n", 2 } }, false);
            });
        } };
        Client client{ srv.Endpoint() };
        auto const sse{ client.Get("/events") };
        check(sse && (*sse)[http::field::content_type] == "text/event-stream" && (*sse)[http::field::cache_control] == "no-cache", "SSE must use text/event-stream without caching");
        check(sse && sse->body() == "retry: 1500\n\nid: 7\nevent: update\ndata: line1\ndata: line2\n\n: ping\n\ndata: bye\n\n", std::format("SSE framing must match the spec, got '{}'", sse ? sse->body() : ""));
        auto const ndjson{ client.Get("/ndjson") };
        check(ndjson && (*ndjson)[http::field::content_type] == "application/x-ndjson" && ndjson->body() == "{\"n\":1}\n{\"n\":2}\n", "NDJSON must write one document per line");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("content setters", test_content_setters);
    run_case("multipart response", test_form_data_response);
    run_case("file serving and ranges", test_file_and_ranges);
    run_case("stream writer", test_stream_writer);
    run_case("SSE and NDJSON writers", test_sse_and_ndjson);
    return finish("test_response");
}
