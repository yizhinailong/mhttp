/**
 * @file test_tls.cpp
 * @brief Verify TLS trust, host identity, pooling, redirects, timeouts, and cancellation.
 */
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import mhttp;

#include "support.hpp"
#include "tls_support.hpp"

using namespace test_support;
using mhttp::client::HttpClient;

namespace {

    /**
     * @brief Run one client scenario with a bounded deadline.
     */
    auto run_client(std::function<net::awaitable<void>(net::io_context&)> body) -> void {
        net::io_context ioc;
        net::steady_timer watchdog{ ioc, std::chrono::seconds{ 10 } };
        bool finished{};
        watchdog.async_wait([&](boost::system::error_code ec) {
            if (!ec) {
                check(false, "TLS scenario exceeded its deadline");
                ioc.stop();
            }
        });
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true;
            watchdog.cancel(); }, net::detached);
        ioc.run();
        check(finished, "TLS scenario must complete");
    }

    /**
     * @brief Verify an authenticated HTTPS connection, SNI, payloads, and reuse.
     */
    auto test_client_transport() -> void {
        TlsIdentity identity;
        RawTlsServer server{ identity };
        auto context{ identity.ClientContext() };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, std::format("https://localhost:{}", server.Port()) };
            client.SetTlsContext(context);
            auto first{ co_await client.AsyncGet("/") };
            check(first && first->AsString() == "secure", "HTTPS must verify the test CA and decode the response");
            std::string const payload(200 * 1024, 's');
            auto second{ co_await client.AsyncPost("/echo", payload, "text/plain") };
            check(second && second->AsString() == payload, "TLS must stream a body spanning many records");
            check(first && second && first->Base()["X-Connection"] == second->Base()["X-Connection"], "HTTPS requests must reuse a connection");
            check(server.SawSni(), "DNS connections must send SNI");
            co_await client.AsyncClose();
            HttpClient address{ ioc, "127.0.0.1", server.Port(), mhttp::url::Scheme::Tls };
            address.SetTlsContext(context);
            check((co_await address.AsyncGet("/")).has_value(), "IP address verification must use the certificate IP SAN");
            co_await address.AsyncClose();
        });
    }

    /**
     * @brief Reject untrusted and mismatched certificates and malformed trust inputs.
     */
    auto test_verification() -> void {
        TlsIdentity identity{ "DNS:wrong.example" };
        RawTlsServer server{ identity };
        auto context{ identity.ClientContext() };
        check(!mhttp::TlsClientContext::Create({ .use_system_ca = false, .ca_pem = "invalid PEM" }), "malformed PEM must fail during context creation");
        check(!mhttp::TlsClientContext::Create({ .ca_file = "mhttp-no-such-ca.pem" }), "a missing CA file must fail during context creation");
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient untrusted{ ioc, "localhost", server.Port(), mhttp::url::Scheme::Tls };
            check(!(co_await untrusted.AsyncGet("/")) && !untrusted.IsOpen(), "default trust must reject a self-signed certificate");
            HttpClient mismatch{ ioc, "localhost", server.Port(), mhttp::url::Scheme::Tls };
            mismatch.SetTlsContext(context);
            check(!(co_await mismatch.AsyncGet("/")) && !mismatch.IsOpen(), "a trusted certificate for another host must be rejected");
            auto insecure{ mhttp::TlsClientContext::Create({ .verify_peer = false }) };
            if (check(insecure.has_value(), "explicit insecure context must be constructible")) {
                mismatch.SetTlsContext(*insecure);
                check((co_await mismatch.AsyncGet("/")).has_value(), "verification may be disabled explicitly for development");
                co_await mismatch.AsyncClose();
            }
        });
    }

    /**
     * @brief Propagate trust through cross-origin redirects and pooled clients.
     */
    auto test_pool_and_redirect() -> void {
        TlsIdentity identity;
        RawTlsServer secure{ identity };
        auto context{ identity.ClientContext() };
        TestServer redirect{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(mhttp::Verb::get, "/", [&](mhttp::Request&, mhttp::Response& response) {
                response.SetRedirect(std::format("https://localhost:{}/", secure.Port()));
            });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", redirect.Endpoint().port() };
            client.SetTlsContext(context);
            client.SetMaxRedirects(1);
            auto response{ co_await client.AsyncGet("/") };
            check(response && response->AsString() == "secure", "cross-origin HTTPS redirects must inherit trust");
            co_await client.AsyncClose();
            mhttp::client::HttpClientPool pool{
                ioc.get_executor(),
                { .validate_on_borrow = true, .tls_context = context }
            };
            auto handle{ co_await pool.AsyncAcquire(std::format("https://localhost:{}", secure.Port())) };
            if (check(static_cast<bool>(handle), "HTTPS pool borrow must succeed")) {
                auto first{ co_await handle->AsyncGet("/") };
                check(first.has_value(), "pool clients must inherit trust");
                handle.Release();
                auto next{ co_await pool.AsyncAcquire(std::format("https://localhost:{}", secure.Port())) };
                if (check(static_cast<bool>(next), "an authenticated connection must be borrowable again")) {
                    auto second{ co_await next->AsyncGet("/") };
                    check(first && second && first->Base()["X-Connection"] == second->Base()["X-Connection"], "idle TLS validation must preserve usable sessions");
                }
            }
            pool.Stop();
            auto rejecting{ mhttp::TlsClientContext::Create({ .use_system_ca = false }) };
            mhttp::client::HttpClientPool locked_pool{ ioc.get_executor(), { .tls_context = *rejecting } };
            auto temporary{ co_await locked_pool.AsyncAcquire("localhost", secure.Port(), mhttp::url::Scheme::Tls) };
            if (check(static_cast<bool>(temporary), "a borrower may configure its own trust")) {
                temporary->SetTlsContext(context);
                check((co_await temporary->AsyncGet("/")).has_value(), "temporary borrower trust must work");
                temporary.Release();
                auto restored{ co_await locked_pool.AsyncAcquire("localhost", secure.Port(), mhttp::url::Scheme::Tls) };
                if (check(static_cast<bool>(restored), "pool configuration must be restored on return")) {
                    check(!(co_await restored->AsyncGet("/")), "a connection authenticated with borrower trust must not bypass the pool's restored trust");
                }
            }
            locked_pool.Stop();
        });
    }

    /**
     * @brief Bound and cancel a TLS handshake that receives no ServerHello.
     */
    auto test_handshake_deadline_and_cancel() -> void {
        net::io_context peer_ioc;
        tcp::acceptor acceptor{
            peer_ioc,
            { tcp::v4(), 0 }
        };
        std::vector<std::shared_ptr<tcp::socket>> peers;
        net::co_spawn(peer_ioc, [&]() -> net::awaitable<void> {
            for (;;) {
                boost::system::error_code ec;
                auto socket{ co_await acceptor.async_accept(net::redirect_error(net::use_awaitable, ec)) };
                if (ec) {
                    co_return;
                }
                peers.push_back(std::make_shared<tcp::socket>(std::move(socket)));
            } }, net::detached);
        std::jthread worker{ [&] { peer_ioc.run(); } };
        auto insecure{ mhttp::TlsClientContext::Create({ .verify_peer = false }) };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient client{ ioc, "127.0.0.1", acceptor.local_endpoint().port(), mhttp::url::Scheme::Tls };
            client.SetTlsContext(*insecure);
            client.SetTimeout(std::chrono::milliseconds{ 100 });
            auto response{ co_await client.AsyncGet("/") };
            check(!response && response.error() == boost::beast::error::timeout, "TLS negotiation must obey the request deadline");
            client.SetTimeout(std::chrono::seconds{ 5 });
            net::steady_timer cancel{ ioc, std::chrono::milliseconds{ 50 } };
            cancel.async_wait([&](boost::system::error_code ec) {
                if (!ec) {
                    static_cast<void>(client.Close());
                }
            });
            response = co_await client.AsyncGet("/");
            check(!response && !client.IsOpen() && !client.HasActiveSession(), "close must cancel TLS negotiation and release the client");
        });
        peer_ioc.stop();
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("HTTPS client transport and SNI", test_client_transport);
    run_case("certificate verification", test_verification);
    run_case("HTTPS pools and redirects", test_pool_and_redirect);
    run_case("TLS handshake deadline and cancellation", test_handshake_deadline_and_cancel);
    return finish("test_tls");
}
