/**
 * @file test_server.cpp
 * @brief Exercise HttpServer over loopback TCP: lifecycle, keep-alive, errors, and shutdown.
 */
#include <boost/asio/awaitable.hpp>
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

namespace {

    /**
     * @brief Register "GET/POST/HEAD /echo" answering "<METHOD> <target> <body>".
     * @param server Server to configure.
     */
    auto register_echo(mhttp::HttpServer& server) -> void {
        server.GetRouter().SetHttpHandler<mhttp::Verb::get, mhttp::Verb::post, mhttp::Verb::head>("/*", [](mhttp::Request& req, mhttp::Response& resp) {
            resp.SetStringContent(std::format("{} {} {}", req.MethodString(), req.Target(), req.AsString()), "text/plain");
        });
    }

    auto test_dispatch_and_keep_alive() -> void {
        TestServer srv{ register_echo };
        Client client{ srv.Endpoint() };
        for (int i{ 0 }; i < 3; ++i) {
            auto const resp{ client.Send(make_request(http::verb::post, std::format("/echo/{}", i), "payload")) };
            check(resp.has_value(), "request must be answered");
            if (resp) {
                check(resp->result() == http::status::ok, "handler status must be sent");
                check(resp->body() == std::format("POST /echo/{} payload", i), "body must echo method, target, and body");
                check(resp->keep_alive(), "HTTP/1.1 responses must stay keep-alive");
                check(!(*resp)[http::field::server].empty() && !(*resp)[http::field::date].empty(), "Server and Date headers must be set");
            }
        }
    }

    auto test_head_matches_get() -> void {
        TestServer srv{ register_echo };
        Client client{ srv.Endpoint() };
        auto const head{ client.Send(make_request(http::verb::head, "/h")) };
        check(head && head->body().empty(), "HEAD must not carry body bytes");
        check(head && (*head)[http::field::content_length] == std::to_string(std::string_view{ "HEAD /h " }.size()), "HEAD must report the GET-identical length");
        auto const get{ client.Get("/after") };
        check(get && get->body() == "GET /after ", "the request after HEAD must parse cleanly");
    }

    auto test_unrouted_is_404() -> void {
        TestServer srv;
        Client client{ srv.Endpoint() };
        for (int i{ 0 }; i < 2; ++i) {
            auto const resp{ client.Get("/missing") };
            check(resp && resp->result() == http::status::not_found, "requests without a route must return 404");
            check(resp && resp->body().find("404 Not Found") != std::string::npos, "404 must carry the HTML error page");
        }
    }

    auto test_handler_exception() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(mhttp::Verb::get, "/throw", [](mhttp::Request&, mhttp::Response&) -> net::awaitable<void> {
                throw std::runtime_error{ "boom" };
                co_return;
            });
        } };
        Client client{ srv.Endpoint() };
        auto const resp{ client.Get("/throw") };
        check(resp && resp->result() == http::status::internal_server_error, "a throwing handler must produce 500");
        check(resp && !resp->keep_alive(), "a 500 from an exception must close the connection");
        check(client.IsClosedByPeer(), "the server must close the connection after the 500");
    }

    auto test_body_limit_is_413() -> void {
        std::atomic<int> calls{ 0 };
        TestServer srv{ [&](mhttp::HttpServer& server) {
            server.SetBodyLimit(16);
            server.GetRouter().SetHttpHandler(mhttp::Verb::post, "/big", [&](mhttp::Request&, mhttp::Response&) { ++calls; });
        } };
        Client client{ srv.Endpoint() };
        auto const resp{ client.Send(make_request(http::verb::post, "/big", std::string(100, 'x'))) };
        check(resp && resp->result() == http::status::payload_too_large, "an oversized body must be answered with 413");
        check(calls.load() == 0, "an oversized body must not reach the handler");
    }

    auto test_header_limit_is_431() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) { server.SetHeaderLimit(256); } };
        Client client{ srv.Endpoint() };
        auto const resp{ client.Get("/", { { "X-Big", std::string(1024, 'a') } }) };
        check(resp && resp->result() == http::status::request_header_fields_too_large, "an oversized header must be answered with 431");
        check(client.IsClosedByPeer(), "the connection must close after 431");
    }

    auto test_malformed_request_is_400() -> void {
        TestServer srv{ register_echo };
        Client client{ srv.Endpoint() };
        client.WriteRaw("GARBAGE\r\n\r\n");
        auto const resp{ client.Read() };
        check(resp && resp->result() == http::status::bad_request, "a malformed request line must be answered with 400");
    }

    auto test_expect_continue() -> void {
        TestServer srv{ register_echo };
        Client client{ srv.Endpoint() };
        client.WriteRaw("POST /c HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n");
        auto const interim{ client.Read() };
        check(interim && interim->result() == http::status::continue_, "Expect: 100-continue must get 100 Continue first");
        client.WriteRaw("hello");
        auto const resp{ client.Read() };
        check(resp && resp->body() == "POST /c hello", "the body sent after 100 Continue must reach the handler");
    }

    auto test_lazy_unread_body_closes() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetLazyHttpHandler(mhttp::Verb::post, "/lazy", [](mhttp::Request&, mhttp::Response& resp) {
                resp.SetStringContent("ignored body", "text/plain");
            });
        } };
        Client client{ srv.Endpoint() };
        // Small enough to fit socket buffers: a blocked client write would mask the result.
        auto const resp{ client.Send(make_request(http::verb::post, "/lazy", std::string(1000, 'z'))) };
        check(resp && resp->body() == "ignored body", "a lazy handler may answer without reading the body");
        check(resp && !resp->keep_alive(), "an unread body must end keep-alive, or it would be parsed as the next request");
    }

    auto test_stop_drains_sessions() -> void {
        TestServer srv{ register_echo };
        Client client{ srv.Endpoint() };
        check(client.Get("/").has_value(), "the request before stop must succeed");

        auto stopped{ srv.server.Stop() };
        check(stopped.wait_for(std::chrono::seconds{ 5 }) == std::future_status::ready, "Stop() must not wait for idle keep-alive connections");
        check(srv.done.wait_for(std::chrono::seconds{ 5 }) == std::future_status::ready, "Run() must finish after Stop()");
        if (srv.done.wait_for(std::chrono::seconds{ 0 }) == std::future_status::ready) {
            auto const ec{ srv.done.get() };
            check(!ec, std::format("a normal stop must not report an error, got: {}", ec.message()));
        }
        check(!srv.server.IsOpen(), "the acceptor must be closed after stop");
        check(client.IsClosedByPeer(), "the aborted session must close its connection");
    }

    auto test_stop_aborts_slow_handler() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(mhttp::Verb::get, "/slow", [](mhttp::Request&, mhttp::Response& resp) -> net::awaitable<void> {
                net::steady_timer timer{ co_await net::this_coro::executor, std::chrono::milliseconds{ 200 } };
                co_await timer.async_wait(net::use_awaitable);
                resp.SetStringContent("late", "text/plain");
            });
        } };
        Client client{ srv.Endpoint() };
        client.WriteRaw("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
        auto stopped{ srv.server.Stop() };
        check(stopped.wait_for(std::chrono::seconds{ 5 }) == std::future_status::ready, "Stop() must drain a session whose handler is still running");
        check(!client.Read().has_value(), "the aborted session must not deliver a response");
    }

    auto test_run_twice() -> void {
        TestServer srv;
        auto second{ srv.server.Run() };
        check(second.wait_for(std::chrono::seconds{ 5 }) == std::future_status::ready, "a second Run() must return immediately");
        check(second.get() == net::error::already_started, "a second Run() must report already_started");
    }

    auto test_listen_conflict() -> void {
        TestServer srv;
        net::io_context ioc;
        mhttp::HttpServer other{ ioc };
        auto const listened{ other.Listen("127.0.0.1", srv.Endpoint().port()) };
        check(!listened.has_value(), "binding an occupied port must fail");
        check(!other.IsOpen(), "a failed Listen() must leave the acceptor closed");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("dispatch and keep-alive", test_dispatch_and_keep_alive);
    run_case("HEAD matches GET", test_head_matches_get);
    run_case("unrouted request is 404", test_unrouted_is_404);
    run_case("handler exception is 500", test_handler_exception);
    run_case("body limit is 413", test_body_limit_is_413);
    run_case("header limit is 431", test_header_limit_is_431);
    run_case("malformed request is 400", test_malformed_request_is_400);
    run_case("Expect: 100-continue", test_expect_continue);
    run_case("lazy route with unread body closes", test_lazy_unread_body_closes);
    run_case("stop drains idle sessions", test_stop_drains_sessions);
    run_case("stop aborts slow handler", test_stop_aborts_slow_handler);
    run_case("second Run() is rejected", test_run_twice);
    run_case("listen conflict reports an error", test_listen_conflict);
    return finish("test_server");
}
