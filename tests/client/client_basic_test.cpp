/**
 * @file client_basic_test.cpp
 * @brief Exercise public client Send against real dynamic-port loopback servers without Asio in consumer code.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

/**
 * @brief Bind and start a configured server on a dynamic IPv4 loopback port.
 * @param server Configured listener, alive through every request.
 * @return Chosen port.
 */
auto start_loopback(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port) { throw std::runtime_error{ "cannot bind client fixture" }; }
    if (!server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start client fixture" }; }
    return *port;
}

/**
 * @brief Report constructor validation without opening sockets and retain issued requests across wrapper destruction.
 */
auto constructors_and_ownership() -> void {
    mhttp::Runtime runtime;
    {
        mhttp::client::HttpClient never_started{ runtime, "localhost", 80 };
        Check(!never_started.IsOpen() && !never_started.InitializationError(), "valid construction performs no I/O on an idle runtime");
    }
    Check(runtime.Start(4).has_value(), "start client constructor runtime");
    for (auto const& [url, error] : std::to_array<std::pair<std::string_view, mhttp::Error>>({
             { "", mhttp::Error::InvalidUrl }, { " \t", mhttp::Error::InvalidUrl },
             { "http://user@host/", mhttp::Error::InvalidUrl }, { "http://host:65536/", mhttp::Error::InvalidUrl },
             { "https://localhost/", mhttp::Error::UnsupportedScheme } })) {
        mhttp::client::HttpClient invalid{ runtime, url };
        CheckEq(invalid.InitializationError(), std::optional{ error }, "constructor retains validation failure");
        auto result = mhttp::SyncWait(runtime, invalid.Send(mhttp::client::Request{}));
        CheckEq(result.error(), error, "Send reports constructor failure without connecting");
        Check(!invalid.IsOpen(), "invalid client never opens a connection");
    }
    mhttp::client::HttpClient normalized{ runtime, "HTTP://Example.COM:8080/a?q=1#local" };
    CheckEq(normalized.Host(), "example.com"sv, "URL constructor owns normalized host");
    CheckEq(normalized.Port(), std::uint16_t{ 8080 }, "URL constructor retains explicit port");
    mhttp::Task<mhttp::Result<mhttp::client::Response>> issued;
    {
        mhttp::client::HttpClient transient{ runtime, "127.0.0.1", 0 };
        issued = transient.Send(mhttp::client::Request{});
    }
    auto canceled = mhttp::SyncWait(runtime, std::move(issued));
    CheckEq(canceled.error(), mhttp::Error::Canceled, "issued Task owns state after destruction requests cancellation");
}

/**
 * @brief Verify common methods, repeated queries, default fields, binary data, endpoints, and connection reuse.
 */
auto send_methods_and_metadata() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start method runtime");
    mhttp::server::HttpServer server{ runtime };
    std::atomic<unsigned> calls{};
    for (auto method : { mhttp::Method::Get, mhttp::Method::Head, mhttp::Method::Post, mhttp::Method::Put,
                         mhttp::Method::Patch, mhttp::Method::Delete, mhttp::Method::Options }) {
        Check(server.Route(method, "/echo", [&calls](auto& request, auto& response) {
            ++calls;
            CheckEq(request.Target(), "/echo?original=1&tag=one+two&tag=a%2Fb"sv, "peer receives ordered escaped query without fragment");
            CheckEq(request.Query().Values("tag"), std::vector{ "one two"sv, "a/b"sv }, "peer decodes repeated additional query values");
            CheckEq(request.Header("X-Default"), "default"sv, "configuration field reaches every request");
            CheckEq(request.Header("X-Override"), "explicit"sv, "explicit fields override defaults");
            Check(!request.Headers().Has("Accept-Encoding") && !request.Headers().Has("Content-Type"), "client infers neither compression nor content type");
            CheckEq(request.Headers().Count("Content-Length"), std::size_t{ 1 }, "only one authoritative request length");
            Check(response.Headers().Set("X-Method", request.MethodString()).has_value(), "return method");
            Check(response.Headers().Set("X-Host", request.Header("Host")).has_value(), "return generated Host");
            Check(response.Headers().Set("X-Agent", request.Header("User-Agent")).has_value(), "return generated agent");
            Check(response.Headers().Set("X-Peer-Port", std::to_string(request.RemoteEndpoint().port)).has_value(), "return actual peer port");
            Check(response.SetContent(request.Body(), "application/octet-stream").has_value(), "echo binary body");
        }).has_value(), "register common method route");
    }
    auto port = start_loopback(server);
    mhttp::client::ClientOptions options;
    Check(options.default_headers.Set("X-Default", "default").has_value(), "default request field");
    Check(options.default_headers.Set("X-Override", "ignored").has_value(), "overridden fallback field");
    mhttp::client::HttpClient client{ runtime, "localhost", port, std::move(options) };
    Check(!client.IsOpen(), "construction remains lazy even with a running runtime");
    std::optional<mhttp::Endpoint> local;
    for (auto method : { mhttp::Method::Get, mhttp::Method::Head, mhttp::Method::Post, mhttp::Method::Put,
                         mhttp::Method::Patch, mhttp::Method::Delete, mhttp::Method::Options }) {
        mhttp::client::Request request{ method, "/echo?original=1#local?ignored=1" };
        request.Query().Add("tag", "one two");
        request.Query().Add("tag", "a/b");
        Check(request.Headers().Set("X-Override", "explicit").has_value(), "explicit request field");
        Check(request.Headers().Add("Content-Length", "wrong").has_value(), "stale framing is ignored");
        Check(request.SetContent(std::string{ "x\0y"sv }).has_value(), "untyped binary content");
        auto task = client.Send(std::move(request));
        if (calls == 0) { Check(!client.IsOpen(), "issuing a lazy Task does not start DNS or connection establishment"); }
        auto response = mhttp::SyncWait(runtime, std::move(task));
        if (!Check(response.has_value(), "real Send completes")) { continue; }
        CheckEq(response->Status(), mhttp::Status::Ok, "HTTP success status");
        CheckEq(response->Body(), method == mhttp::Method::Head ? ""sv : "x\0y"sv, "HEAD suppresses wire content while other methods retain NUL bytes");
        CheckEq(response->Header("X-Method"), mhttp::ToString(method), "all methods reach their handlers");
        CheckEq(response->Header("X-Host"), std::format("localhost:{}", port), "Host uses constructor name and dynamic port");
        CheckEq(response->Header("X-Agent"), "mhttp/" + std::string{ mhttp::VERSION }, "built-in agent reaches actual wire");
        CheckEq(response->RemoteEndpoint(), mhttp::Endpoint{ "127.0.0.1", port }, "client snapshots actual IPv4 endpoint after hostname resolution");
        CheckEq(response->Header("X-Peer-Port"), std::to_string(response->LocalEndpoint().port), "response local snapshot matches server peer socket");
        if (local) { CheckEq(response->LocalEndpoint(), *local, "sequential requests reuse the same TCP connection"); }
        else { local = response->LocalEndpoint(); }
        Check(response->KeepAlive() && client.IsOpen(), "successful framed response preserves connection");
    }
    CheckEq(calls.load(), 7U, "all common methods complete exactly once");
    client.Stop();
    Check(!client.IsOpen(), "Stop immediately clears published connection state");
}

/**
 * @brief Treat HTTP error/bodyless statuses as responses and honor URL default target and response closure.
 */
auto statuses_and_url_target() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start status runtime");
    mhttp::server::HttpServer server{ runtime };
    for (auto status : { mhttp::Status::NoContent, mhttp::Status::NotModified, mhttp::Status::NotFound }) {
        Check(server.Get('/' + std::to_string(static_cast<unsigned>(status)), [status](auto&, auto& response) {
            Check(response.SetEmpty(status).has_value(), "select ordinary bodyless/error status");
        }).has_value(), "register status response");
    }
    Check(server.Get("/base", [](auto& request, auto& response) {
        CheckEq(request.Target(), "/base?from=url"sv, "empty request target uses URL path and query without its fragment");
        Check(response.Headers().Set("Connection", "close").has_value(), "explicit closing response");
        Check(response.SetContent("base", "text/plain").has_value(), "URL target content");
    }).has_value(), "register constructor target");
    auto port = start_loopback(server);
    mhttp::client::HttpClient client{ runtime, std::format("http://127.0.0.1:{}/base?from=url#local", port) };
    for (auto status : { mhttp::Status::NoContent, mhttp::Status::NotModified, mhttp::Status::NotFound }) {
        auto response = mhttp::SyncWait(runtime, client.Send(mhttp::client::Request{
            mhttp::Method::Get, '/' + std::to_string(static_cast<unsigned>(status)) }));
        Check(response && response->Status() == status && response->Body().empty(), "HTTP status is never a transport error");
    }
    for (unsigned i{}; i < 2; ++i) {
        auto response = mhttp::SyncWait(runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "" }));
        Check(response && response->Body() == "base" && !response->KeepAlive(), "closing URL response completes normally");
        Check(!client.IsOpen(), "response close discards connection before next Send");
    }
}

/**
 * @brief Reject overlapping Send, cancel from a foreign thread, and allow subsequent requests to reconnect.
 */
auto busy_and_foreign_stop() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start concurrent client runtime");
    mhttp::server::HttpServer server{ runtime };
    std::latch entered{ 4 };
    Check(server.Get("/slow", [&entered](auto&, auto&) -> mhttp::Task<void> {
        entered.count_down();
        co_await mhttp::Sleep(5s);
    }).has_value(), "register pending requests");
    Check(server.Get("/fast", [](auto&, auto& response) {
        Check(response.SetContent("fast", "text/plain").has_value(), "reconnected response");
    }).has_value(), "register fast route");
    auto port = start_loopback(server);
    std::array<std::unique_ptr<mhttp::client::HttpClient>, 4> clients;
    std::array<std::optional<mhttp::Result<mhttp::client::Response>>, 4> results;
    std::vector<std::jthread> waiters;
    for (std::size_t i{}; i < clients.size(); ++i) {
        clients[i] = std::make_unique<mhttp::client::HttpClient>(runtime, "127.0.0.1", port);
        waiters.emplace_back([&runtime, &clients, &results, i] {
            results[i].emplace(mhttp::SyncWait(runtime, clients[i]->Send(mhttp::client::Request{ mhttp::Method::Get, "/slow" })));
        });
    }
    entered.wait();
    auto overlapping = mhttp::SyncWait(runtime, clients[0]->Send(mhttp::client::Request{ mhttp::Method::Get, "/fast" }));
    CheckEq(overlapping.error(), mhttp::Error::Busy, "second request returns Busy without interrupting the first");
    auto const started = std::chrono::steady_clock::now();
    for (auto& client : clients) { client->Stop(); }
    for (auto& waiter : waiters) { waiter.join(); }
    for (auto& result : results) { Check(result && !result->has_value() && result->error() == mhttp::Error::Canceled, "foreign Stop cancels every in-flight client"); }
    Check(std::chrono::steady_clock::now() - started < 1s, "Stop does not wait for the five-second handlers");
    auto next = mhttp::SyncWait(runtime, clients[0]->Send(mhttp::client::Request{ mhttp::Method::Get, "/fast" }));
    Check(next && next->Body() == "fast", "new request issued after Stop can reconnect");
}

/**
 * @brief Bound an entire response read and close idle sockets when their runtime generation stops.
 */
auto deadlines_and_runtime_restart() -> void {
    mhttp::Runtime server_runtime, client_runtime;
    Check(server_runtime.Start(4).has_value() && client_runtime.Start(4).has_value(), "start independent client/server runtimes");
    mhttp::server::HttpServer server{ server_runtime };
    Check(server.Get("/slow", [](auto&, auto&) -> mhttp::Task<void> { co_await mhttp::Sleep(300ms); }).has_value(), "register delayed response");
    Check(server.Get("/fast", [](auto&, auto& response) {
        Check(response.SetContent("ready", "text/plain").has_value(), "fast response after timeout");
    }).has_value(), "register successful response");
    auto port = start_loopback(server);
    mhttp::client::ClientOptions options{ .read_timeout = 50ms };
    mhttp::client::HttpClient client{ client_runtime, "127.0.0.1", port, options };
    auto const started = std::chrono::steady_clock::now();
    auto timeout = mhttp::SyncWait(client_runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "/slow" }));
    CheckEq(timeout.error(), mhttp::Error::ReadTimeout, "slow handler triggers a real client read deadline");
    Check(std::chrono::steady_clock::now() - started < 250ms && !client.IsOpen(), "timed-out connection closes before handler completion");
    auto first = mhttp::SyncWait(client_runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "/fast" }));
    Check(first && first->Body() == "ready" && client.IsOpen(), "next request reconnects after timeout");
    client_runtime.Stop();
    Check(client_runtime.Join().has_value() && !client.IsOpen(), "runtime stop closes an idle client and drains callback ownership");
    Check(client_runtime.Start(4).has_value(), "restart client execution generation");
    auto next = mhttp::SyncWait(client_runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "/fast" }));
    Check(next && next->Body() == "ready", "same client reconnects under a fresh runtime generation");
    if (first && next) { Check(first->LocalEndpoint() != next->LocalEndpoint(), "runtime restart creates a fresh TCP connection"); }
}

/**
 * @brief Cancel an active Send through its runtime generation, drain it, and reuse the same client after restart.
 */
auto active_runtime_stop() -> void {
    mhttp::Runtime server_runtime, client_runtime;
    Check(server_runtime.Start(4).has_value() && client_runtime.Start(4).has_value(), "start active-cancellation runtimes");
    mhttp::server::HttpServer server{ server_runtime };
    std::promise<void> entered;
    auto ready = entered.get_future();
    Check(server.Get("/hold", [&entered](auto&, auto&) -> mhttp::Task<void> {
        entered.set_value();
        co_await mhttp::Sleep(5s);
    }).has_value(), "register active runtime-stop request");
    Check(server.Get("/fast", [](auto&, auto& response) {
        Check(response.SetContent("new-generation", "text/plain").has_value(), "response after runtime restart");
    }).has_value(), "register restarted request");
    auto port = start_loopback(server);
    mhttp::client::HttpClient client{ client_runtime, "127.0.0.1", port };
    std::optional<mhttp::Result<mhttp::client::Response>> result;
    std::jthread waiter{ [&] {
        result.emplace(mhttp::SyncWait(client_runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "/hold" })));
    } };
    if (!Check(ready.wait_for(1s) == std::future_status::ready, "request reaches server before runtime cancellation")) { std::abort(); }
    auto const started = std::chrono::steady_clock::now();
    client_runtime.Stop();
    Check(client_runtime.Join().has_value(), "drain active client cancellation before restart");
    waiter.join();
    Check(result && !result->has_value() && result->error() == mhttp::Error::Canceled, "runtime stop becomes an expected Canceled result");
    Check(!client.IsOpen() && std::chrono::steady_clock::now() - started < 1s, "runtime stop closes socket without waiting for server handler");
    Check(client_runtime.Start(4).has_value(), "start client runtime after active cancellation");
    auto next = mhttp::SyncWait(client_runtime, client.Send(mhttp::client::Request{ mhttp::Method::Get, "/fast" }));
    Check(next && next->Body() == "new-generation", "same client can reconnect after active runtime cancellation");
}

/**
 * @brief Round-trip all byte values in a four-MiB representation and retain the response after network owners drain.
 */
auto large_binary_ownership() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start large binary runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Post("/binary", [](auto& request, auto& response) {
        CheckEq(request.Body().size(), std::size_t{ 4 * 1024 * 1024 }, "server owns complete bounded upload");
        Check(response.SetContent(request.Body(), "application/octet-stream").has_value(), "echo four-MiB binary body");
    }).has_value(), "register large binary echo");
    auto port = start_loopback(server);
    std::string binary(4 * 1024 * 1024, '\0');
    for (std::size_t i{}; i < binary.size(); ++i) { binary[i] = static_cast<char>(i % 256); }
    std::optional<mhttp::client::Response> saved;
    {
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
        mhttp::client::Request upload{ mhttp::Method::Post, "/binary" };
        Check(upload.SetContent(binary, "application/octet-stream").has_value(), "own binary source");
        auto response = mhttp::SyncWait(runtime, client.Send(std::move(upload)));
        if (Check(response.has_value(), "large body completes in both directions")) { saved.emplace(std::move(*response)); }
    }
    Check(server.Stop().has_value(), "stop binary server");
    runtime.Stop();
    Check(runtime.Join().has_value(), "drain every binary socket and callback");
    if (saved) { CheckEq(saved->Body(), binary, "response owns all byte values after client/server/runtime activity ends"); }
}

/**
 * @brief Exercise both convenience overload groups and preserve explicit field precedence before actual sends.
 */
auto convenience_requests() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start convenience runtime");
    mhttp::server::HttpServer server{ runtime };
    std::atomic<unsigned> calls{};
    for (auto method : { mhttp::Method::Get, mhttp::Method::Head, mhttp::Method::Post, mhttp::Method::Put,
                         mhttp::Method::Patch, mhttp::Method::Delete, mhttp::Method::Options }) {
        Check(server.Route(method, "/shortcuts", [&calls](auto& request, auto& response) {
            ++calls;
            Check(response.Headers().Set("X-Method", request.MethodString()).has_value(), "return convenience method");
            Check(response.Headers().Set("X-Target", request.Target()).has_value(), "return convenience target");
            Check(response.Headers().Set("X-Size", std::to_string(request.Body().size())).has_value(), "return convenience body size");
            Check(response.Headers().Set("X-Type", request.ContentType()).has_value(), "return convenience media type");
            Check(response.SetContent(request.Body(), "application/octet-stream").has_value(), "return convenience binary content");
        }).has_value(), "register convenience method");
    }
    auto port = start_loopback(server);
    mhttp::client::HttpClient client{ runtime, std::format("http://127.0.0.1:{}/shortcuts?tag=from-url", port) };
    auto from_url = mhttp::SyncWait(runtime, client.Get());
    Check(from_url && from_url->Header("X-Target") == "/shortcuts?tag=from-url", "empty shortcut path uses constructor URL");
    mhttp::QueryParams query;
    query.Add("tag", "one two");
    mhttp::Headers headers;
    Check(headers.Set("Content-Type", "application/explicit").has_value(), "explicit convenience media type");
    std::string binary{ "x\0y"sv };
    using Operation = std::function<mhttp::Task<mhttp::Result<mhttp::client::Response>>()>;
    std::vector<std::pair<mhttp::Method, Operation>> operations{
        { mhttp::Method::Get, [&] { return client.Get("/shortcuts", query, headers); } },
        { mhttp::Method::Head, [&] { return client.Head("/shortcuts", query, headers); } },
        { mhttp::Method::Post, [&] { return client.Post("/shortcuts", query, headers); } },
        { mhttp::Method::Put, [&] { return client.Put("/shortcuts", query, headers); } },
        { mhttp::Method::Patch, [&] { return client.Patch("/shortcuts", query, headers); } },
        { mhttp::Method::Delete, [&] { return client.Delete("/shortcuts", query, headers); } },
        { mhttp::Method::Options, [&] { return client.Options("/shortcuts", query, headers); } },
        { mhttp::Method::Get, [&] { return client.Get("/shortcuts", binary, "text/plain", query, headers); } },
        { mhttp::Method::Head, [&] { return client.Head("/shortcuts", binary, "text/plain", query, headers); } },
        { mhttp::Method::Post, [&] { return client.Post("/shortcuts", binary, "text/plain", query, headers); } },
        { mhttp::Method::Put, [&] { return client.Put("/shortcuts", binary, "text/plain", query, headers); } },
        { mhttp::Method::Patch, [&] { return client.Patch("/shortcuts", binary, "text/plain", query, headers); } },
        { mhttp::Method::Delete, [&] { return client.Delete("/shortcuts", binary, "text/plain", query, headers); } },
        { mhttp::Method::Options, [&] { return client.Options("/shortcuts", binary, "text/plain", query, headers); } },
    };
    std::size_t index{};
    for (auto& [method, operation] : operations) {
        auto response = mhttp::SyncWait(runtime, operation());
        if (Check(response.has_value(), "convenience overload completes over TCP")) {
            CheckEq(response->Header("X-Method"), mhttp::ToString(method), "shortcut selects its actual HTTP method");
            CheckEq(response->Header("X-Target"), "/shortcuts?tag=one+two"sv, "shortcut owns and appends ordered parameters");
            CheckEq(response->Header("X-Type"), "application/explicit"sv, "explicit Content-Type wins over optional fallback");
            CheckEq(response->Header("X-Size"), index < 7 ? "0"sv : "3"sv, "body overload transfers all binary bytes including for HEAD");
            CheckEq(response->Body(), index < 7 || method == mhttp::Method::Head ? ""sv : "x\0y"sv, "response follows method body semantics");
        }
        ++index;
    }
    auto fallback = mhttp::SyncWait(runtime, client.Post("/shortcuts", binary, "text/plain"));
    Check(fallback && fallback->Header("X-Type") == "text/plain", "fallback media type is used when explicit field is absent");
    Check(headers.Set("Content-Type", "").has_value(), "explicit empty Content-Type");
    auto empty = mhttp::SyncWait(runtime, client.Post("/shortcuts", binary, "text/plain", {}, headers));
    Check(empty && empty->Header("X-Type").empty(), "explicit empty media type is preserved");
    auto const before = calls.load();
    auto invalid_type = mhttp::SyncWait(runtime, client.Post("/shortcuts", "body", "text/plain\r\nInjected: yes"));
    CheckEq(invalid_type.error(), mhttp::Error::InvalidHeaders, "shortcut rejects unsafe fallback media type before I/O");
    mhttp::client::Request invalid_host{ mhttp::Method::Get, "/shortcuts" };
    Check(invalid_host.Headers().Add("Host", "one").has_value() && invalid_host.Headers().Add("host", "two").has_value(), "invalid duplicate Host setup");
    auto rejected = mhttp::SyncWait(runtime, client.Send(std::move(invalid_host)));
    CheckEq(rejected.error(), mhttp::Error::InvalidHeaders, "Send rejects semantically invalid fields before writing a prefix");
    CheckEq(calls.load(), before, "preflight rejections dispatch no requests");
    auto next = mhttp::SyncWait(runtime, client.Get());
    Check(next && empty && next->LocalEndpoint() == empty->LocalEndpoint(), "preflight failures leave the existing TCP stream unmodified and reusable");
}

/**
 * @brief Run bounded public-client loopback acceptance cases.
 * @param argc Argument count.
 * @param argv Optional test filters.
 * @return Test process status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client constructors and ownership", constructors_and_ownership },
        { "client Send methods and metadata", send_methods_and_metadata },
        { "client statuses and URL target", statuses_and_url_target },
        { "client Busy and foreign Stop", busy_and_foreign_stop },
        { "client deadlines and runtime restart", deadlines_and_runtime_restart },
        { "client active runtime stop", active_runtime_stop },
        { "client large binary ownership", large_binary_ownership },
        { "client convenience requests", convenience_requests },
    });
}
