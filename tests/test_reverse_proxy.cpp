/**
 * @file test_reverse_proxy.cpp
 * @brief Exercise reverse proxy routing, streaming, HTTP framing, failures, and cancellation.
 */
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
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
using mhttp::client::BodyMode;
using mhttp::client::HttpClient;
using mhttp::client::LazyRequest;

namespace {

    /**
     * @brief Yield without blocking either real server's executor.
     */
    auto delay(std::chrono::milliseconds duration) -> net::awaitable<void> {
        net::steady_timer timer{ co_await net::this_coro::executor, duration };
        co_await timer.async_wait(net::use_awaitable);
    }

    /**
     * @brief Bound client tests so missing chunks or cancellation cannot hang the test program.
     */
    auto run_client(std::function<net::awaitable<void>(net::io_context&)> const& body) -> void {
        net::io_context ioc;
        net::steady_timer watchdog{ ioc, std::chrono::seconds{ 8 } };
        bool finished{ false };
        watchdog.async_wait([&](boost::system::error_code ec) { if (!ec) { ioc.stop(); } });
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true;
            watchdog.cancel(); }, net::detached);
        ioc.run();
        check(finished, "the proxy client must complete before the watchdog expires");
    }

    /**
     * @brief Build a configured local origin URL.
     */
    auto origin(TestServer const& server) -> std::string {
        return std::format("http://127.0.0.1:{}", server.Endpoint().port());
    }

    /**
     * @brief Emit one exact HTTP response so framing is independent of mhttp's response writer.
     */
    class RawUpstream {
    private:
        net::io_context m_ioc; ///< Raw server context.
        tcp::acceptor m_acceptor{
            m_ioc,
            tcp::endpoint{ net::ip::address_v4::loopback(), 0 }
        };                     ///< Listener.
        std::jthread m_worker; ///< Joined before the executor is destroyed.

    public:
        /**
         * @brief Accept one request and write the supplied wire bytes.
         */
        explicit RawUpstream(std::string wire) {
            net::co_spawn(m_ioc, [this, wire = std::move(wire)]() -> net::awaitable<void> {
                boost::system::error_code ec;
                auto socket{ co_await m_acceptor.async_accept(net::redirect_error(net::use_awaitable, ec)) };
                boost::beast::flat_buffer buffer;
                http::request_parser<http::empty_body> request;
                co_await http::async_read_header(socket, buffer, request, net::redirect_error(net::use_awaitable, ec));
                if (!ec) { co_await net::async_write(socket, net::buffer(wire), net::redirect_error(net::use_awaitable, ec)); }
                socket.shutdown(tcp::socket::shutdown_send, ec);
                socket.close(ec); }, net::detached);
            m_worker = std::jthread{ [this] { m_ioc.run(); } };
        }

        /**
         * @brief Stop an uncontacted listener as well as a completed one.
         */
        ~RawUpstream() { m_ioc.stop(); }

        /**
         * @brief Return this raw upstream's HTTP origin.
         */
        auto Url() const -> std::string {
            return std::format("http://127.0.0.1:{}", m_acceptor.local_endpoint().port());
        }
    };

    /**
     * @brief Replace encoded path prefixes, keep the query and method, and reuse upstream sockets.
     */
    auto test_routes_and_headers() -> void {
        TestServer upstream{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler<Verb::get, Verb::head, Verb::post, Verb::put, Verb::patch, Verb::delete_, Verb::options, Verb::trace>("/*", [](mhttp::Request& request, mhttp::Response& response) {
                mhttp::Json info;
                info["target"] = request.Target();
                info["method"] = request.MethodString();
                info["host"] = request[Field::host];
                info["peer"] = request.RemoteEndpoint().port();
                info["authorization"] = request[Field::authorization];
                info["proxy_auth"] = request[Field::proxy_authorization];
                info["hop1"] = request["X-Hop-One"];
                info["hop2"] = request["X-Hop-Two"];
                info["forwarded"] = request["Forwarded"];
                info["xff"] = request["X-Forwarded-For"];
                info["xfh"] = request["X-Forwarded-Host"];
                info["xfp"] = request["X-Forwarded-Proto"];
                info["body"] = request.Type() == mhttp::BodyType::String ? request.AsString() : std::string{};
                response.SetJsonContent(info);
                response.Base().Set("Connection", "X-Origin-One");
                response.Base().Insert("Connection", "X-Origin-Two");
                response.Base().Set("X-Origin-One", "secret");
                response.Base().Set("X-Origin-Two", "secret");
                response.Base().Insert(Field::set_cookie, "a=1");
                response.Base().Insert(Field::set_cookie, "b=2");
            });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            server.SetReverseProxy("/api/", origin(upstream) + "/backend/");
            mhttp::ReverseProxyConfig config;
            config.strip_prefix = false;
            config.preserve_host = true;
            server.SetReverseProxy("/keep", origin(upstream), config);
            server.GetRouter().SetHttpHandler(Verb::get, "/api/local", [](mhttp::Request&, mhttp::Response& response) { response.SetStringContent("local", "text/plain"); });
        } };
        Client client{ proxy.Endpoint() };
        std::optional<std::uint16_t> previous_peer;
        for (auto const& [path, expected] : std::array<std::pair<std::string_view, std::string_view>, 4>{
                 std::pair{ "/api/a%2Fb?x=%2F&x=2", "/backend/a%2Fb?x=%2F&x=2" },
                 std::pair{         "/%61pi/item?",           "/backend/item?" },
                 std::pair{                 "/api",                "/backend/" },
                 std::pair{                "/api/",                "/backend/" }
        }) {
            auto request{
                make_request(Verb::get, path, {},
                  { { "Host", "public.example" }, { "Authorization", "Bearer token" }, { "Proxy-Authorization", "secret" }, { "X-Hop-One", "secret" }, { "X-Hop-Two", "secret" }, { "Forwarded", "for=spoof" }, { "X-Forwarded-For", "spoof" }, { "X-Forwarded-Proto", "https" } }
                  )
            };
            request.set(Field::connection, "X-Hop-One");
            request.insert(Field::connection, "X-Hop-Two");
            auto const response{ client.Send(request) };
            if (!check(response && response->result() == Status::ok, "the prefix route must return the upstream response")) {
                continue;
            }
            auto info = mhttp::Json::parse(response->body());
            check(info["target"].get<std::string>() == expected && info["host"] == std::format("127.0.0.1:{}", upstream.Endpoint().port()), "rewrite must preserve reserved encoding and query while replacing Host");
            check(info["authorization"] == "Bearer token" && info["proxy_auth"] == "" && info["hop1"] == "" && info["hop2"] == "", "both Connection nominations and proxy credentials must be removed");
            check(info["forwarded"] == "" && info["xff"] == "127.0.0.1" && info["xfh"] == "public.example" && info["xfp"] == "http", "forwarding metadata must not trust client-supplied values");
            check(response->count("X-Origin-One") == 0 && response->count("X-Origin-Two") == 0 && response->count(Field::set_cookie) == 2, "response hop fields must be removed while duplicate cookies survive");
            auto const peer{ info["peer"].get<std::uint16_t>() };
            if (previous_peer) {
                check(*previous_peer == peer, "completed exchanges must reuse the upstream connection");
            }
            previous_peer = peer;
        }
        for (auto const method : { Verb::post, Verb::put, Verb::patch, Verb::delete_, Verb::options, Verb::trace }) {
            std::string const payload{ method == Verb::trace ? "" : "payload" };
            auto request{
                make_request(method, "/api/verbs", payload, { { "Content-Type", "application/octet-stream" } }
                   )
            };
            auto const response{ client.Send(request) };
            if (check(response && response->result() == Status::ok, "each registered HTTP method must relay")) {
                auto info = mhttp::Json::parse(response->body());
                check(info["body"] == payload && info["method"].get<std::string>() == std::string_view{ http::to_string(method) }, "the method and fixed-length upload must be preserved");
            }
        }
        auto const kept{
            client.Get("/keep/path", { { "Host", "public.example" } }
             )
        };
        if (check(kept.has_value(), "the preserve-prefix route must respond")) {
            auto info = mhttp::Json::parse(kept->body());
            check(info["target"] == "/keep/path" && info["host"] == "public.example", "prefix and Host preservation options must apply");
        }
        auto const local{ client.Get("/api/local") };
        check(local && local->body() == "local", "a more specific local route must override the wildcard proxy");
        auto const outside{ client.Get("/api-other") };
        check(outside && outside->result() == Status::not_found, "prefix matching must respect path segment boundaries");
        client.WriteRaw("GET /api/one HTTP/1.1\r\nHost: public.example\r\n\r\nGET /api/two HTTP/1.1\r\nHost: public.example\r\n\r\n");
        auto const one{ client.Read() };
        auto const two{ client.Read() };
        check(one && two && mhttp::Json::parse(one->body())["target"] == "/backend/one" && mhttp::Json::parse(two->body())["target"] == "/backend/two", "peer monitoring must not consume a pipelined request");
        client.WriteRaw("POST /api/continue HTTP/1.1\r\nHost: public.example\r\nExpect: 100-continue\r\nContent-Length: 7\r\nContent-Type: text/plain\r\n\r\n");
        auto const interim{ client.Read() };
        check(interim && interim->result() == Status::continue_, "the proxy must acknowledge Expect before reading the upload");
        client.WriteRaw("payload");
        auto const continued{ client.Read() };
        check(continued && continued->result() == Status::ok && mhttp::Json::parse(continued->body())["body"] == "payload", "a continued upload must reach the upstream without another Expect handshake");
    }

    /**
     * @brief Forward the first upload chunk and SSE event while the remaining body is withheld.
     */
    auto test_streaming() -> void {
        std::atomic<bool> first_upload{ false };
        std::atomic<bool> release_response{ false };
        TestServer upstream{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetLazyHttpHandler(Verb::post, "/upload", [&](mhttp::Request& request, mhttp::Response& response) -> net::awaitable<void> {
                std::array<char, 4096> buffer{};
                std::size_t total{ 0 };
                boost::system::error_code ec;
                while (!request.IsBodyDone()) {
                    total += co_await request.ReadSomeRaw(net::buffer(buffer), ec);
                    if (ec) {
                        co_return;
                    }
                    first_upload.store(true);
                }
                response.SetStringContent(std::to_string(total), "text/plain");
            });
            server.GetRouter().SetHttpHandler(Verb::get, "/events", [&](mhttp::Request&, mhttp::Response& response) -> net::awaitable<void> {
                auto stream{ response.CreateStreamWriter() };
                mhttp::Headers headers;
                headers.Set(Field::content_type, "text/event-stream");
                boost::system::error_code ec;
                co_await stream->WriteHeader(Status::ok, headers, mhttp::StreamWriter::Mode::Chunked, ec);
                co_await stream->WriteBody(net::buffer(std::string_view{ "data: first\n\n" }), true, ec);
                while (!release_response.load()) {
                    co_await delay(std::chrono::milliseconds{ 5 });
                }
                co_await stream->WriteBody(net::buffer(std::string_view{ "data: last\n\n" }), false, ec);
            });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetReverseProxy("/", origin(upstream)); } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, origin(proxy) };
            auto writer{ client.CreateLazyRequest() };
            boost::system::error_code ec;
            co_await writer->WriteHeader(Verb::post, "/upload", mhttp::Headers{}, LazyRequest::Mode::Chunked, ec);
            std::string const chunk(65536, 'x');
            co_await writer->WriteBody(net::buffer(chunk), true, ec);
            for (int i{ 0 }; i < 200 && !first_upload.load(); ++i) {
                co_await delay(std::chrono::milliseconds{ 2 });
            }
            check(first_upload.load(), "the first upload chunk must arrive before the upload ends");
            for (int i{ 0 }; i < 31; ++i) {
                co_await writer->WriteBody(net::buffer(chunk), true, ec);
            }
            co_await writer->WriteBody({}, false, ec);
            auto uploaded{ co_await writer->ReadResponse() };
            check(uploaded && uploaded->AsString() == "2097152", "a large chunked upload must retain its complete payload");
            writer.reset();
            auto events{ co_await client.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/events" }, BodyMode::Lazy) };
            if (check(events.has_value(), "the proxy must expose streaming response headers")) {
                auto reader{ events->CreateSseReader() };
                auto first{ co_await reader->ReadEvent() };
                check(first && *first && (**first).data == "first" && !release_response.load(), "the first SSE event must arrive before the upstream finishes");
                release_response.store(true);
                auto last{ co_await reader->ReadEvent() };
                auto end{ co_await reader->ReadEvent() };
                check(last && *last && (**last).data == "last" && end && !*end, "the final event and chunk terminator must be relayed");
            }
            release_response.store(true);
        });
        release_response.store(true);
    }

    /**
     * @brief HEAD and bodyless responses must leave downstream keep-alive framing intact.
     */
    auto test_empty_and_head() -> void {
        TestServer upstream{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler<Verb::get, Verb::head>("/resource", [](mhttp::Request&, mhttp::Response& response) { response.SetStringContent("resource", "text/plain"); });
            server.GetRouter().SetHttpHandler(Verb::get, "/empty", [](mhttp::Request&, mhttp::Response& response) { response.SetEmptyContent(Status::no_content); });
            server.GetRouter().SetHttpHandler(Verb::get, "/cached", [](mhttp::Request&, mhttp::Response& response) { response.SetEmptyContent(Status::not_modified); });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetReverseProxy("/api", origin(upstream)); } };
        Client client{ proxy.Endpoint() };
        auto const head{ client.Send(make_request(Verb::head, "/api/resource")) };
        check(head && head->result() == Status::ok && head->body().empty() && (*head)[Field::content_length] == "8", "HEAD must preserve the representation length without emitting body bytes");
        for (auto const& [path, status] : {
                 std::pair{  "/api/empty",   Status::no_content },
                 std::pair{ "/api/cached", Status::not_modified }
        }) {
            auto const result{ client.Get(path) };
            check(result && result->result() == status && result->body().empty() && result->count(Field::transfer_encoding) == 0, "bodyless statuses must not emit chunks on the keep-alive connection");
        }
        auto const next{ client.Get("/api/resource") };
        check(next && next->body() == "resource", "a request after HEAD and bodyless responses must parse normally");
    }

    /**
     * @brief Map upstream connection failures and response-header deadlines to gateway errors.
     */
    auto test_errors() -> void {
        TestServer upstream{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/slow", [](mhttp::Request&, mhttp::Response& response) -> net::awaitable<void> {
                co_await delay(std::chrono::milliseconds{ 250 });
                response.SetStringContent("late", "text/plain");
            });
            server.GetRouter().SetHttpHandler(Verb::get, "/truncated", [](mhttp::Request&, mhttp::Response& response) -> net::awaitable<void> {
                mhttp::Headers headers;
                headers.Set(Field::content_length, "100");
                auto writer{ response.CreateStreamWriter() };
                boost::system::error_code ec;
                co_await writer->WriteHeader(Status::ok, headers, mhttp::StreamWriter::Mode::Relay, ec);
                co_await writer->WriteBody(net::buffer(std::string_view{ "short" }), true, ec);
            });
        } };
        net::io_context unused;
        tcp::acceptor closed{
            unused,
            tcp::endpoint{ net::ip::address_v4::loopback(), 0 }
        };
        auto const unused_port{ closed.local_endpoint().port() };
        closed.close();
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::ReverseProxyConfig config;
            config.timeout = std::chrono::milliseconds{ 60 };
            server.SetReverseProxy("/api", origin(upstream), config);
            server.SetReverseProxy("/offline", std::format("http://127.0.0.1:{}", unused_port));
        } };
        {
            Client client{ proxy.Endpoint() };
            auto const response{ client.Get("/offline") };
            check(response && response->result() == Status::bad_gateway, "connection refusal must produce 502");
        }
        {
            Client client{ proxy.Endpoint() };
            auto const response{ client.Get("/api/slow") };
            check(response && response->result() == Status::gateway_timeout, "a response-header timeout must produce 504");
        }
        {
            Client client{ proxy.Endpoint() };
            auto const response{ client.Get("/api/truncated") };
            check(!response, "an upstream body failure after headers must truncate the downstream response");
        }
    }

    /**
     * @brief Reframe close-delimited and chunked origins, including HTTP/1.0 downstreams.
     */
    auto test_wire_framing() -> void {
        std::string const legacy{ "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nlegacy body" };
        std::string const chunks{ "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-End\r\n\r\n3\r\none\r\n3\r\ntwo\r\n0\r\nX-End: ignored\r\n\r\n" };
        for (int scenario{ 0 }; scenario < 3; ++scenario) {
            RawUpstream upstream{ scenario == 0 ? legacy : chunks };
            TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetReverseProxy("/", upstream.Url()); } };
            Client client{ proxy.Endpoint() };
            auto request{ make_request(Verb::get, "/") };
            if (scenario == 2) {
                request.version(10);
                request.keep_alive(true);
            }
            auto const result{ client.Send(request) };
            check(result && result->body() == (scenario == 0 ? "legacy body" : "onetwo"), "the proxy must preserve decoded payload across each framing conversion");
            if (result) {
                check(result->count(Field::trailer) == 0 && result->count("X-End") == 0 && result->chunked() == (scenario != 2), "trailers must be dropped and chunking must match the downstream HTTP version");
            }
        }
    }

    /**
     * @brief Reject oversized headers, oversized bodies, upgrades, and unsupported transfer codings.
     */
    auto test_response_limits() -> void {
        for (int scenario{ 0 }; scenario < 4; ++scenario) {
            std::string wire;
            if (scenario == 0) {
                wire = "HTTP/1.1 200 OK\r\nX-Large: " + std::string(1024, 'x') + "\r\nContent-Length: 0\r\n\r\n";
            } else if (scenario == 1) {
                wire = "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\ntoo large";
            } else if (scenario == 2) {
                wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
            } else {
                wire = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
            }
            RawUpstream upstream{ wire };
            TestServer proxy{ [&](mhttp::HttpServer& server) {
                mhttp::ReverseProxyConfig config;
                config.header_limit = 128;
                config.body_limit = 8;
                server.SetReverseProxy("/", upstream.Url(), config);
            } };
            Client client{ proxy.Endpoint() };
            auto const result{ client.Get("/") };
            check(result && result->result() == Status::bad_gateway, "invalid or oversized upstream responses must fail before downstream headers are sent");
        }
    }

    /**
     * @brief Stop cancels both a header read and a request waiting for the sole pool connection.
     */
    auto test_stop() -> void {
        std::atomic<bool> entered{ false };
        std::atomic<bool> release{ false };
        TestServer upstream{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/slow", [&](mhttp::Request&, mhttp::Response& response) -> net::awaitable<void> {
                entered.store(true);
                while (!release.load()) {
                    co_await delay(std::chrono::milliseconds{ 5 });
                }
                response.SetEmptyContent(Status::ok);
            });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::ReverseProxyConfig config;
            config.max_connections = 1;
            config.acquire_timeout = std::chrono::seconds{ 20 };
            server.SetReverseProxy("/", origin(upstream), config);
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient first{ ioc, origin(proxy) };
            HttpClient second{ ioc, origin(proxy) };
            int done{ 0 };
            net::co_spawn(ioc, [&]() -> net::awaitable<void> { static_cast<void>(co_await first.AsyncGet("/slow")); ++done; }, net::detached);
            while (!entered.load()) {
                co_await delay(std::chrono::milliseconds{ 2 });
            }
            net::co_spawn(ioc, [&]() -> net::awaitable<void> { static_cast<void>(co_await second.AsyncGet("/slow")); ++done; }, net::detached);
            co_await delay(std::chrono::milliseconds{ 50 });
            auto stopped{ proxy.server.Stop() };
            for (int i{ 0 }; i < 500 && stopped.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready; ++i) {
                co_await delay(std::chrono::milliseconds{ 2 });
            }
            check(stopped.wait_for(std::chrono::seconds{ 0 }) == std::future_status::ready, "Stop must cancel upstream I/O and pool waits without waiting for their long deadlines");
            release.store(true);
            while (done != 2) {
                co_await delay(std::chrono::milliseconds{ 2 });
            }
        });
        release.store(true);
    }

    /**
     * @brief A downstream disconnect releases the upstream pool slot before the origin responds.
     */
    auto test_disconnect() -> void {
        std::atomic<bool> entered{ false };
        std::atomic<bool> release{ false };
        TestServer upstream{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/slow", [&](mhttp::Request&, mhttp::Response& response) -> net::awaitable<void> {
                entered.store(true);
                while (!release.load()) {
                    co_await delay(std::chrono::milliseconds{ 5 });
                }
                response.SetEmptyContent(Status::ok);
            });
            server.GetRouter().SetHttpHandler(Verb::get, "/fast", [](mhttp::Request&, mhttp::Response& response) { response.SetStringContent("fast", "text/plain"); });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::ReverseProxyConfig config;
            config.max_connections = 1;
            config.acquire_timeout = std::chrono::milliseconds{ 500 };
            server.SetReverseProxy("/", origin(upstream), config);
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient first{ ioc, origin(proxy) };
            bool done{ false };
            net::co_spawn(ioc, [&]() -> net::awaitable<void> { static_cast<void>(co_await first.AsyncGet("/slow")); done = true; }, net::detached);
            while (!entered.load()) {
                co_await delay(std::chrono::milliseconds{ 2 });
            }
            co_await first.AsyncClose();
            HttpClient second{ ioc, origin(proxy) };
            auto result{ co_await second.AsyncGet("/fast") };
            check(result && result->Result() == Status::ok && result->AsString() == "fast" && !release.load(), "a disconnected client must release capacity before the stalled origin finishes");
            release.store(true);
            while (!done) {
                co_await delay(std::chrono::milliseconds{ 2 });
            }
        });
        release.store(true);
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("routes and headers", test_routes_and_headers);
    run_case("streaming upload and SSE", test_streaming);
    run_case("empty and HEAD", test_empty_and_head);
    run_case("gateway errors", test_errors);
    run_case("wire framing", test_wire_framing);
    run_case("response limits", test_response_limits);
    run_case("stop and pool wait", test_stop);
    run_case("downstream disconnect", test_disconnect);
    return finish("test_reverse_proxy");
}
