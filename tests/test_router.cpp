/**
 * @file test_router.cpp
 * @brief Verify route matching, path parameters, middleware, and routing hooks.
 */
#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import mhttp;

#include "support.hpp"

using namespace test_support;
using mhttp::Request;
using mhttp::Response;
using mhttp::Verb;

namespace {

    /**
     * @brief Answer with a fixed text.
     * @param text Body text.
     * @return Handler.
     */
    auto reply(std::string text) {
        return [text = std::move(text)](Request&, Response& resp) { resp.SetStringContent(text, "text/plain"); };
    }

    auto test_match_priority() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::get, "/users/*", reply("wildcard"));
            router.SetHttpHandler(Verb::get, "/users/:id", [](Request& req, Response& resp) { resp.SetStringContent(std::format("param {}", req.PathParam("id")), "text/plain"); });
            router.SetHttpHandler(Verb::get, "/users/{id:\\d+}", [](Request& req, Response& resp) { resp.SetStringContent(std::format("regex {}", req.PathParam<int>("id") + 1), "text/plain"); });
            router.SetHttpHandler(Verb::get, "/users/me", reply("static"));
            router.SetHttpHandler(Verb::get, "/", reply("root"));
        } };
        Client client{ srv.Endpoint() };
        auto body{ [&](std::string_view target) {
            auto const resp{ client.Get(target) };
            return resp ? resp->body() : std::string{ "<no response>" };
        } };
        check(body("/users/me") == "static", "static segments must win");
        check(body("/users/41") == "regex 42", "regex segments must beat parameters and convert typed params");
        check(body("/users/alice") == "param alice", "parameters must beat wildcards");
        check(body("/users/a/b/c") == "wildcard", "wildcards must consume the remaining segments");
        check(body("/") == "root", "the root route must match");
    }

    auto test_wildcard_and_decoding() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/files/*", [](Request& req, Response& resp) { resp.SetStringContent(std::string{ req.PathParam("*") }, "text/plain"); });
            server.GetRouter().SetHttpHandler(Verb::get, "/a b/:name", [](Request& req, Response& resp) { resp.SetStringContent(std::string{ req.PathParam("name") }, "text/plain"); });
            server.GetRouter().SetHttpHandler(Verb::get, "/api/secret", reply("secret"));
        } };
        Client client{ srv.Endpoint() };
        auto const wildcard{ client.Get("/files/x/y.txt?q=1") };
        check(wildcard && wildcard->body() == "x/y.txt", "the wildcard parameter must hold the decoded remainder without the query");
        auto const decoded{ client.Get("/a%20b/caf%C3%A9") };
        check(decoded && decoded->body() == "caf\xC3\xA9", "segments must be percent-decoded before matching and capture");
        auto const encoded_slash{ client.Get("/api%2Fsecret") };
        check(encoded_slash && encoded_slash->result() == http::status::not_found, "%2F must stay inside its segment, not split the path");
    }

    auto test_method_not_allowed() -> void {
        TestServer srv{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler<Verb::get, Verb::post>("/item", reply("item"));
        } };
        Client client{ srv.Endpoint() };
        auto const resp{ client.Send(make_request(http::verb::delete_, "/item")) };
        check(resp && resp->result() == http::status::method_not_allowed, "a known path with another method must be 405");
        check(resp && (*resp)[http::field::allow] == "GET,POST", "405 must list the registered methods in Allow");
        auto const post{ client.Send(make_request(http::verb::post, "/item", "x")) };
        check(post && post->body() == "item", "multi-verb registration must register every method");
    }

    auto test_not_found_and_post_routing() -> void {
        std::atomic<int> post_routing_calls{ 0 };
        TestServer srv{ [&](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetHttpHandler(Verb::get, "/ok", reply("ok"));
            router.SetHttpNotFoundHandler([](Request& req, Response& resp) { resp.SetStringContent(std::format("custom 404 for {}", req.Path()), "text/plain", mhttp::Status::not_found); });
            router.SetPostRoutingHandler([&](Request&, Response& resp) -> net::awaitable<void> {
                ++post_routing_calls;
                resp.Set("X-Post-Routing", "1");
                co_return;
            });
        } };
        Client client{ srv.Endpoint() };
        auto const missing{ client.Get("/nope") };
        check(missing && missing->result() == http::status::not_found && missing->body() == "custom 404 for /nope", "the not-found handler must answer unmatched paths");
        auto const ok{ client.Get("/ok") };
        check(ok && (*ok)["X-Post-Routing"] == "1", "the post-routing hook must run after handlers");
        check(post_routing_calls.load() == 2, "the post-routing hook must run for matched and unmatched requests");
    }

    /**
     * @brief Aspect that records hook order and optionally stops the chain.
     */
    struct Recorder {
        std::shared_ptr<std::vector<std::string>> log; ///< Shared event log.
        std::string name;                              ///< Label in the log.
        bool allow{ true };                            ///< Result of Before().

        auto Before(Request&, Response& resp) -> bool {
            log->push_back(name + ".before");
            if (!allow) {
                resp.SetStringContent("blocked by " + name, "text/plain", mhttp::Status::forbidden);
            }
            return allow;
        }

        auto After(Request&, Response&) -> net::awaitable<bool> {
            log->push_back(name + ".after");
            co_return true;
        }
    };

    auto test_aspects() -> void {
        auto log{ std::make_shared<std::vector<std::string>>() };
        TestServer srv{ [&](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.Use(Recorder{ log, "global" });
            router.SetHttpHandler(
                Verb::get, "/chain", [log](Request&, Response& resp) {
                    log->push_back("handler");
                    resp.SetStringContent("chain", "text/plain");
                },
                Recorder{ log, "outer" }, Recorder{ log, "inner" }
            );
            router.SetHttpHandler(Verb::get, "/blocked", reply("unreachable"), Recorder{ log, "guard", false });
        } };
        Client client{ srv.Endpoint() };
        auto const chained{ client.Get("/chain") };
        check(chained && chained->body() == "chain", "the handler must run when every Before allows it");
        std::vector<std::string> const expected{ "global.before", "outer.before", "inner.before", "handler", "outer.after", "inner.after", "global.after" };
        check(*log == expected, std::format("hooks must run global-outer-inner around the handler, got {}", *log));

        log->clear();
        auto const blocked{ client.Get("/blocked") };
        check(blocked && blocked->result() == http::status::forbidden && blocked->body() == "blocked by guard", "a false Before must short-circuit the handler");
        check(std::ranges::find(*log, std::string{ "guard.after" }) != log->end(), "After hooks still run after a short-circuit, as in httplib");
    }

    /**
     * @brief Non-copyable stateful aspect shared by reference.
     */
    struct Counter {
        int count{ 0 };

        Counter() = default;
        Counter(Counter const&) = delete;
        auto operator=(Counter const&) -> Counter& = delete;

        auto Before(Request&, Response&) -> bool {
            ++count;
            return true;
        }
    };

    auto test_borrowed_aspect() -> void {
        Counter counter;
        TestServer srv{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler<Verb::get>("/a", reply("a"), std::ref(counter));
            server.GetRouter().SetHttpHandler<Verb::get>("/b", reply("b"), std::ref(counter));
        } };
        Client client{ srv.Endpoint() };
        client.Get("/a");
        client.Get("/b");
        client.Get("/a");
        check(counter.count == 3, "std::ref aspects must be shared, not copied");
    }

    /**
     * @brief Handler object registered through a member function pointer.
     */
    struct Controller {
        std::string greeting{ "hello" };

        auto Greet(Request& req, Response& resp) -> net::awaitable<void> {
            resp.SetStringContent(std::format("{} {}", greeting, req.Query().At("name")), "text/plain");
            co_return;
        }
    };

    auto test_member_handler() -> void {
        Controller controller;
        TestServer srv{ [&](mhttp::HttpServer& server) { server.GetRouter().SetHttpHandler<Verb::get>("/greet", &Controller::Greet, controller); } };
        Client client{ srv.Endpoint() };
        auto const resp{ client.Get("/greet?name=bob") };
        check(resp && resp->body() == "hello bob", "member function handlers must be bound to their owner");
    }

    auto test_registration_errors() -> void {
        mhttp::Router router;
        bool rejected{ false };
        try {
            router.SetHttpHandler(Verb::get, "/x/{bad:(a+)+}", reply("x"));
        } catch (std::invalid_argument const&) {
            rejected = true;
        }
        check(rejected, "nested quantifiers must be rejected at registration (ReDoS guard)");

        rejected = false;
        try {
            router.SetHttpHandler(Verb::get, std::format("/x/{{long:{}}}", std::string(300, 'a')), reply("x"));
        } catch (std::invalid_argument const&) {
            rejected = true;
        }
        check(rejected, "overlong regex patterns must be rejected");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("match priority", test_match_priority);
    run_case("wildcard and decoding", test_wildcard_and_decoding);
    run_case("405 with Allow", test_method_not_allowed);
    run_case("not-found and post-routing hooks", test_not_found_and_post_routing);
    run_case("aspect order and short-circuit", test_aspects);
    run_case("borrowed aspect", test_borrowed_aspect);
    run_case("member function handler", test_member_handler);
    run_case("registration errors", test_registration_errors);
    return finish("test_router");
}
