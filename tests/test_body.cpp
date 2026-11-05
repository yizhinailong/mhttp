/**
 * @file test_body.cpp
 * @brief Verify request parsing: query, headers, typed bodies, multipart, lazy streaming, client IP.
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

    /**
     * @brief Build a multipart/form-data body.
     * @param boundary Boundary token.
     * @param parts (name, filename, content) triples; empty filename for plain fields.
     * @return Encoded body.
     */
    auto multipart_body(
        std::string_view boundary,
        std::vector<std::tuple<std::string, std::string, std::string>> const& parts
    ) -> std::string {
        std::string body;
        for (auto const& [name, filename, content] : parts) {
            body += std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"", boundary, name);
            if (!filename.empty()) {
                body += std::format("; filename=\"{}\"\r\nContent-Type: application/octet-stream", filename);
            }
            body += std::format("\r\n\r\n{}\r\n", content);
        }
        body += std::format("--{}--\r\n", boundary);
        return body;
    }

    auto test_query_and_headers() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/q", [](Request& req, Response& resp) {
                auto const& query{ req.Query() };
                std::string lines{ std::format("a={} n={} flag={} tags={} sp={}", query.At("a"), query.At<int>("n") * 2, query.At<bool>("flag"), query.All("tag").size(), query.At("sp")) };
                lines += std::format(" x-custom={} count={}", req["X-Custom"], req.Count("X-Multi"));
                std::size_t header_lines{ 0 };
                for (auto const& header : req.Base()) {
                    header_lines += header.name_string.empty() ? 0 : 1;
                }
                lines += std::format(" headers>={}", header_lines >= 3);
                resp.SetStringContent(std::move(lines), "text/plain");
            });
        } };
        Client client{ srv.Endpoint() };
        auto req{ make_request(http::verb::get, "/q?a=x%20y&n=21&flag=true&tag=1&tag=2&sp=a+b") };
        req.set("X-Custom", "hello");
        req.insert("X-Multi", "1");
        req.insert("X-Multi", "2");
        auto const resp{ client.Send(req) };
        check(resp && resp->body() == "a=x y n=42 flag=true tags=2 sp=a b x-custom=hello count=2 headers>=true", std::format("query and headers must be decoded, got '{}'", resp ? resp->body() : ""));
    }

    auto test_typed_bodies() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::post, "/json", [](Request& req, Response& resp) {
                auto const& json{ req.AsJson() };
                resp.SetJsonContent(mhttp::Json{ { "type", static_cast<int>(req.Type()) }, { "sum", json["a"].get<int>() + json["b"].get<int>() } });
            });
            router.SetHttpHandler(Verb::post, "/form", [](Request& req, Response& resp) {
                auto const& params{ req.AsQueryParams() };
                resp.SetStringContent(std::format("{}|{}", params.At("name"), params.At("city")), "text/plain");
            });
            router.SetHttpHandler(Verb::post, "/text", [](Request& req, Response& resp) {
                resp.SetStringContent(std::format("{}:{}", static_cast<int>(req.Type()), req.AsString()), "text/plain");
            });
            router.SetHttpHandler(Verb::get, "/empty", [](Request& req, Response& resp) {
                resp.SetStringContent(std::format("{}:{}", req.Type() == mhttp::BodyType::Empty, req.AsString().empty()), "text/plain");
            });
        } };
        Client client{ srv.Endpoint() };
        auto const json{ client.Send(make_request(http::verb::post, "/json", R"({"a":40,"b":2})", { { "Content-Type", "application/json" } })) };
        check(json && nlohmann::json::parse(json->body()) == nlohmann::json{ { "type", static_cast<int>(mhttp::BodyType::Json) }, { "sum", 42 } }, "JSON bodies must materialize as Json");
        check(json && (*json)[http::field::content_type].starts_with("application/json"), "JSON responses must set Content-Type");

        auto const bad_json{ client.Send(make_request(http::verb::post, "/json", "{not json", { { "Content-Type", "application/json" } })) };
        check(bad_json && bad_json->result() == http::status::bad_request, "invalid JSON must be answered with 400");

        Client client2{ srv.Endpoint() };
        auto const form{ client2.Send(make_request(http::verb::post, "/form", "name=J%C3%BCrgen+M&city=a%26b", { { "Content-Type", "application/x-www-form-urlencoded" } })) };
        check(form && form->body() == "J\xC3\xBCrgen M|a&b", "form-urlencoded bodies must decode '+' and escapes");
        auto const text{ client2.Send(make_request(http::verb::post, "/text", "raw text", { { "Content-Type", "text/plain" } })) };
        check(text && text->body() == std::format("{}:raw text", static_cast<int>(mhttp::BodyType::String)), "other content types must materialize as strings");
        auto const empty{ client2.Get("/empty") };
        check(empty && empty->body() == "true:true", "body-less requests must read as Empty and as an empty string");
    }

    auto test_multipart() -> void {
        TempDir uploads;
        std::atomic<bool> file_existed_during_request{ false };
        std::filesystem::path seen_path;
        TestServer srv{ [&](mhttp::HttpServer& server) {
            server.SetFormDataConfig(mhttp::FormData::Param{ .save_dir = uploads.path, .max_file_size = 64, .remove_uploaded_files = true });
            server.GetRouter().SetHttpHandler(Verb::post, "/upload", [&](Request& req, Response& resp) {
                auto const& form{ req.AsFormData() };
                auto const file{ form.FieldByName("file") };
                if (file && file->file_path) {
                    seen_path = *file->file_path;
                    file_existed_during_request = std::filesystem::exists(*file->file_path);
                }
                resp.SetStringContent(std::format("{}|{}|{}", form.fields.size(), form.Content("title").value_or("?"), file ? file->filename : "?"), "text/plain");
            });
        } };
        Client client{ srv.Endpoint() };
        auto const body{ multipart_body("XyZ", { { "title", "", "hello world" }, { "file", "../evil.txt", "file content" } }) };
        auto const resp{ client.Send(make_request(http::verb::post, "/upload", body, { { "Content-Type", "multipart/form-data; boundary=XyZ" } })) };
        check(resp && resp->body() == "2|hello world|../evil.txt", std::format("multipart fields must parse, got '{}'", resp ? resp->body() : ""));
        check(file_existed_during_request.load(), "file parts must be written to save_dir while the handler runs");
        check(seen_path.parent_path() == uploads.path, "saved files must stay inside save_dir despite '../' in the filename");

        // The server destroys the first request before it reads the next one on this connection,
        // so the cleanup is observable once the second response arrives.
        auto const too_big{ client.Send(make_request(http::verb::post, "/upload", multipart_body("XyZ", { { "file", "a.bin", std::string(100, 'x') } }), { { "Content-Type", "multipart/form-data; boundary=XyZ" } })) };
        check(too_big && too_big->result() == http::status::payload_too_large, "a part over max_file_size must be answered with 413");
        check(!seen_path.empty() && !std::filesystem::exists(seen_path), "remove_uploaded_files must delete files after the request");
        check(std::filesystem::is_empty(uploads.path), "a part rejected by max_file_size must not leave a file behind");
    }

    auto test_lazy_streaming() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetLazyHttpHandler(Verb::post, "/stream", [](Request& req, Response& resp) -> net::awaitable<void> {
                std::array<char, 7> buffer{};
                std::size_t total{ 0 };
                std::size_t reads{ 0 };
                for (;;) {
                    auto const n{ co_await req.ReadSomeRaw(net::buffer(buffer)) };
                    if (n == 0) {
                        break;
                    }
                    total += n;
                    ++reads;
                }
                boost::system::error_code ec;
                try {
                    co_await req.ReadString();
                } catch (boost::system::system_error const& e) {
                    ec = e.code();
                }
                resp.SetStringContent(std::format("{} {} {} {}", total, reads > 1, req.IsBodyDone(), ec == mhttp::Error::BodyAlreadyStreamed), "text/plain");
            });
            router.SetLazyHttpHandler(Verb::post, "/json", [](Request& req, Response& resp) -> net::awaitable<void> {
                auto const json = co_await req.ReadJson();
                resp.SetStringContent(json.at("k").get<std::string>(), "text/plain");
            });
        } };
        Client client{ srv.Endpoint() };
        // Chunked upload: the handler sees it in small raw reads.
        client.WriteRaw("POST /stream HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
        auto const streamed{ client.Read() };
        check(streamed && streamed->body() == "11 true true true", std::format("lazy raw reads must deliver the de-chunked body, got '{}'", streamed ? streamed->body() : ""));
        check(streamed && streamed->keep_alive(), "a fully read lazy body must keep the connection alive");
        auto const json{ client.Send(make_request(http::verb::post, "/json", R"({"k":"v"})")) };
        check(json && json->body() == "v", "lazy handlers may materialize with ReadJson()");
    }

    auto test_request_data() -> void {
        struct Auth {
            auto Before(Request& req, Response&) -> bool {
                req.Data().Store("user", std::string{ "alice" });
                req.Data().Store(42);
                return true;
            }
        };
        TestServer srv{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/me", [](Request& req, Response& resp) {
                auto const user{ req.Data().Fetch<std::string>("user") };
                auto const number{ req.Data().Fetch<int const&>() };
                resp.SetStringContent(std::format("{} {} {}", user.value_or("?"), number.value_or(0), req.Data().Has<double>()), "text/plain");
            }, Auth{});
        } };
        Client client{ srv.Endpoint() };
        auto const resp{ client.Get("/me") };
        check(resp && resp->body() == "alice 42 false", "request data must pass values from aspects to handlers");
    }

    auto test_client_ip() -> void {
        auto const handler{ [](Request& req, Response& resp) { resp.SetStringContent(req.GetClientIp().to_string(), "text/plain"); } };
        {
            TestServer srv{ [&](mhttp::HttpServer& server) { server.GetRouter().SetHttpHandler(Verb::get, "/ip", handler); } };
            Client client{ srv.Endpoint() };
            auto const resp{ client.Get("/ip", { { "X-Forwarded-For", "1.2.3.4" } }) };
            check(resp && resp->body() == "127.0.0.1", "X-Forwarded-For must be ignored without trusted proxies");
        }
        {
            TestServer srv{ [&](mhttp::HttpServer& server) {
                server.SetTrustedProxies({ "127.0.0.1", "10.0.0.0/8" });
                server.GetRouter().SetHttpHandler(Verb::get, "/ip", handler);
            } };
            Client client{ srv.Endpoint() };
            auto const resp{ client.Get("/ip", { { "X-Forwarded-For", "6.6.6.6, 203.0.113.9, 10.1.2.3" } }) };
            check(resp && resp->body() == "203.0.113.9", "the rightmost untrusted hop must be the client");
        }
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("query string and headers", test_query_and_headers);
    run_case("typed bodies", test_typed_bodies);
    run_case("multipart uploads", test_multipart);
    run_case("lazy streaming", test_lazy_streaming);
    run_case("request data", test_request_data);
    run_case("client IP", test_client_ip);
    return finish("test_body");
}
