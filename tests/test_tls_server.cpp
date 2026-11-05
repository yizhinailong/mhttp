/**
 * @file test_tls_server.cpp
 * @brief Verify HTTPS listener configuration, requests, clean shutdown, and stalled handshakes.
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
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import mhttp;

#include "support.hpp"
#include "tls_support.hpp"

using namespace test_support;

namespace {

    /**
     * @brief Reject invalid identities at configuration time and accept PEM files.
     */
    auto test_identity_configuration() -> void {
        TlsIdentity identity;
        TlsIdentity other;
        check(!mhttp::TlsServerContext::Create({}), "missing identity must be rejected");
        check(!mhttp::TlsServerContext::Create({ .certificate_chain_pem = "invalid", .private_key_pem = identity.key }), "invalid certificate PEM must be rejected");
        check(!mhttp::TlsServerContext::Create({ .certificate_chain_pem = identity.certificate, .private_key_pem = other.key }), "a mismatched private key must be rejected");
        TempDir directory;
        auto const certificate{ directory.path / "server.pem" };
        auto const key{ directory.path / "key.pem" };
        std::ofstream{ certificate } << identity.certificate;
        std::ofstream{ key } << identity.key;
        check(mhttp::TlsServerContext::Create({ .certificate_chain_file = certificate.string(), .private_key_file = key.string() }).has_value(), "server identity files must load");
        check(mhttp::TlsClientContext::Create({ .use_system_ca = false, .ca_file = certificate.string() }).has_value(), "client CA bundles must load from files");
    }

    /**
     * @brief Use an independent Asio peer to verify HTTPS responses and TLS close_notify.
     */
    auto test_https_and_clean_close() -> void {
        TlsIdentity identity;
        TestServer server{ [&](mhttp::HttpServer& listener) {
            listener.SetTlsContext(identity.ServerContext());
            listener.GetRouter().SetHttpHandler(mhttp::Verb::post, "/echo", [](mhttp::Request& request, mhttp::Response& response) {
                response.Set("X-TLS", request.IsTls() ? "yes" : "no");
                response.SetStringContent(request.AsString(), "text/plain");
            });
        } };
        net::io_context ioc;
        net::ssl::context context{ net::ssl::context::tls_client };
        context.add_certificate_authority(net::buffer(identity.certificate));
        net::ssl::stream<tcp::socket> stream{ ioc, context };
        stream.set_verify_mode(net::ssl::verify_peer);
        stream.set_verify_callback(net::ssl::host_name_verification{ "127.0.0.1" });
        stream.next_layer().connect(server.Endpoint());
        stream.handshake(net::ssl::stream_base::client);
        HttpRequest request{ http::verb::post, "/echo", 11 };
        request.set(http::field::host, "localhost");
        request.set(http::field::content_type, "text/plain");
        request.body() = std::string(128 * 1024, 'e');
        request.prepare_payload();
        boost::beast::flat_buffer buffer;
        http::write(stream, request);
        HttpResponse first;
        http::read(stream, buffer, first);
        check(first.result() == http::status::ok && first.body() == request.body() && first["X-TLS"] == "yes", "HTTPS must preserve bodies and expose the actual encrypted transport");
        request.keep_alive(false);
        http::write(stream, request);
        HttpResponse second;
        http::read(stream, buffer, second);
        check(second.body() == request.body(), "HTTPS keep-alive must support another request");
        std::array<char, 1> data{};
        boost::system::error_code ec;
        stream.read_some(net::buffer(data), ec);
        check(ec == net::error::eof, "a normal HTTPS close must send close_notify, not truncate TLS");
        stream.shutdown(ec);
    }

    /**
     * @brief Time out a peer that connects without sending a TLS ClientHello.
     */
    auto test_handshake_timeout_and_stop() -> void {
        TlsIdentity identity;
        TestServer server{ [&](mhttp::HttpServer& listener) {
            listener.SetTlsContext(identity.ServerContext());
            listener.SetReadTimeout(std::chrono::milliseconds{ 100 });
        } };
        net::io_context ioc;
        tcp::socket stalled{ ioc };
        stalled.connect(server.Endpoint());
        std::this_thread::sleep_for(std::chrono::milliseconds{ 250 });
        stalled.non_blocking(true);
        std::array<char, 1> data{};
        boost::system::error_code ec;
        stalled.receive(net::buffer(data), 0, ec);
        check(ec == net::error::eof || ec == net::error::connection_reset, "the server read deadline must close a stalled TLS handshake");
        server.server.SetReadTimeout(std::chrono::seconds{ 30 });
        tcp::socket pending{ ioc };
        pending.connect(server.Endpoint());
        std::this_thread::sleep_for(std::chrono::milliseconds{ 20 });
        auto stopped{ server.server.Stop() };
        check(stopped.wait_for(std::chrono::seconds{ 1 }) == std::future_status::ready, "stop must cancel and drain TLS handshakes without waiting for their deadline");
        stopped.get();
    }

    /**
     * @brief Preserve plaintext behavior and expose encryption independently of spoofed headers.
     */
    auto test_plain_transport_metadata() -> void {
        TestServer server{ [](mhttp::HttpServer& listener) {
            listener.GetRouter().SetHttpHandler(mhttp::Verb::get, "/", [](mhttp::Request& request, mhttp::Response& response) {
                response.SetStringContent(request.IsTls() ? "tls" : "plain", "text/plain");
            });
        } };
        Client client{ server.Endpoint() };
        HttpRequest request{ http::verb::get, "/", 11 };
        request.set("X-Forwarded-Proto", "https");
        auto result{ client.Send(request) };
        check(result && result->body() == "plain", "forwarded metadata must not override the accepted transport");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("TLS server identity configuration", test_identity_configuration);
    run_case("HTTPS body, reuse, and clean close", test_https_and_clean_close);
    run_case("TLS server handshake timeout and stop", test_handshake_timeout_and_stop);
    run_case("plain transport metadata", test_plain_transport_metadata);
    return finish("test_tls_server");
}
