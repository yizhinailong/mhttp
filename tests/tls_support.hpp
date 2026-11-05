/**
 * @file tls_support.hpp
 * @brief Generate ephemeral loopback certificates and run a raw TLS HTTP peer.
 * @note Include after Boost/OpenSSL headers, imports, and support.hpp.
 */
#pragma once

namespace test_support {

    /**
     * @brief An ephemeral self-signed test root and server identity, never written to disk.
     */
    struct TlsIdentity {
        std::string certificate; ///< PEM certificate with caller-selected subject alternatives.
        std::string key;         ///< PEM private key generated afresh for this test process.

        /**
         * @brief Generate a certificate valid around the current clock time.
         */
        explicit TlsIdentity(std::string_view names = "DNS:localhost,IP:127.0.0.1") {
            std::unique_ptr<EVP_PKEY_CTX, decltype(&::EVP_PKEY_CTX_free)> generator{ ::EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), ::EVP_PKEY_CTX_free };
            EVP_PKEY* raw_key{};
            if (!generator || ::EVP_PKEY_keygen_init(generator.get()) != 1 || ::EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) != 1 || ::EVP_PKEY_keygen(generator.get(), &raw_key) != 1) {
                throw std::runtime_error{ "test key generation failed" };
            }
            std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)> private_key{ raw_key, ::EVP_PKEY_free };
            std::unique_ptr<X509, decltype(&::X509_free)> cert{ ::X509_new(), ::X509_free };
            ::X509_set_version(cert.get(), 2);
            ::ASN1_INTEGER_set(::X509_get_serialNumber(cert.get()), 1);
            ::X509_gmtime_adj(::X509_getm_notBefore(cert.get()), -60);
            ::X509_gmtime_adj(::X509_getm_notAfter(cert.get()), 24 * 60 * 60);
            ::X509_set_pubkey(cert.get(), private_key.get());
            auto* subject{ ::X509_get_subject_name(cert.get()) };
            ::X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<unsigned char const*>("mhttp loopback test"), -1, -1, 0);
            ::X509_set_issuer_name(cert.get(), subject);
            X509V3_CTX extensions{};
            ::X509V3_set_ctx(&extensions, cert.get(), cert.get(), nullptr, nullptr, 0);
            auto const add_extension{ [&](int id, std::string const& text) {
                std::unique_ptr<X509_EXTENSION, decltype(&::X509_EXTENSION_free)> extension{ ::X509V3_EXT_conf_nid(nullptr, &extensions, id, text.c_str()), ::X509_EXTENSION_free };
                if (!extension || ::X509_add_ext(cert.get(), extension.get(), -1) != 1) {
                    throw std::runtime_error{ "test certificate extension failed" };
                }
            } };
            add_extension(NID_basic_constraints, "critical,CA:TRUE");
            add_extension(NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
            add_extension(NID_subject_alt_name, std::string{ names });
            if (::X509_sign(cert.get(), private_key.get(), ::EVP_sha256()) == 0) {
                throw std::runtime_error{ "test certificate signing failed" };
            }
            std::unique_ptr<BIO, decltype(&::BIO_free)> cert_bio{ ::BIO_new(::BIO_s_mem()), ::BIO_free };
            std::unique_ptr<BIO, decltype(&::BIO_free)> key_bio{ ::BIO_new(::BIO_s_mem()), ::BIO_free };
            ::PEM_write_bio_X509(cert_bio.get(), cert.get());
            ::PEM_write_bio_PrivateKey(key_bio.get(), private_key.get(), nullptr, nullptr, 0, nullptr, nullptr);
            char* data{};
            auto length{ ::BIO_get_mem_data(cert_bio.get(), &data) };
            certificate.assign(data, static_cast<std::size_t>(length));
            length = ::BIO_get_mem_data(key_bio.get(), &data);
            key.assign(data, static_cast<std::size_t>(length));
        }

        /**
         * @brief Trust only this test certificate.
         */
        auto ClientContext() const -> std::shared_ptr<mhttp::TlsClientContext> {
            auto result{ mhttp::TlsClientContext::Create({ .use_system_ca = false, .ca_pem = certificate }) };
            if (!result) {
                throw std::runtime_error{ result.error().message() };
            }
            return *result;
        }

        /**
         * @brief Load this identity for an mhttp HTTPS listener.
         */
        auto ServerContext() const -> std::shared_ptr<mhttp::TlsServerContext> {
            auto result{ mhttp::TlsServerContext::Create({ .certificate_chain_pem = certificate, .private_key_pem = key }) };
            if (!result) {
                throw std::runtime_error{ result.error().message() };
            }
            return *result;
        }
    };

    /**
     * @brief Independent TLS HTTP peer used to verify the library's client transport.
     */
    class RawTlsServer {
    private:
        net::io_context m_ioc;                                        ///< Peer execution context.
        net::ssl::context m_context{ net::ssl::context::tls_server }; ///< Server identity.
        tcp::acceptor m_acceptor{
            m_ioc,
            { tcp::v4(), 0 }
        };                                        ///< Loopback listener.
        std::atomic<unsigned> m_connections{ 0 }; ///< Completed handshakes.
        std::atomic<bool> m_sni{ false };         ///< Observed localhost SNI.
        std::atomic<bool> m_hold_seen{ false };   ///< Stalled request arrived.
        std::atomic<bool> m_peer_closed{ false }; ///< Stalled connection was cancelled.
        std::jthread m_worker;                    ///< Runs peer coroutines.

    public:
        /**
         * @brief Start a peer that echoes request bodies and identifies its connection.
         */
        explicit RawTlsServer(TlsIdentity const& identity) {
            m_context.use_certificate_chain(net::buffer(identity.certificate));
            m_context.use_private_key(net::buffer(identity.key), net::ssl::context::pem);
            net::co_spawn(m_ioc, [this]() -> net::awaitable<void> {
                for (;;) {
                    boost::system::error_code ec;
                    auto socket{ co_await m_acceptor.async_accept(net::redirect_error(net::use_awaitable, ec)) };
                    if (ec) {
                        co_return;
                    }
                    net::co_spawn(m_ioc, serve(std::move(socket)), net::detached);
                } }, net::detached);
            m_worker = std::jthread{ [this] { m_ioc.run(); } };
        }

        ~RawTlsServer() { m_ioc.stop(); }

        [[nodiscard]] auto Port() const -> std::uint16_t {
            return m_acceptor.local_endpoint().port();
        }

        [[nodiscard]] auto SawSni() const -> bool { return m_sni.load(); }

        /**
         * @brief Report that a held request is ready for cancellation.
         */
        [[nodiscard]] auto SawHold() const -> bool { return m_hold_seen.load(); }

        /**
         * @brief Report closure of the connection serving the held request.
         */
        [[nodiscard]] auto SawDisconnect() const -> bool { return m_peer_closed.load(); }

    private:
        /**
         * @brief Serve one connection until EOF or a caller asks for a close.
         */
        auto serve(tcp::socket socket) -> net::awaitable<void> {
            net::ssl::stream<tcp::socket> stream{ std::move(socket), m_context };
            boost::system::error_code ec;
            co_await stream.async_handshake(net::ssl::stream_base::server, net::redirect_error(net::use_awaitable, ec));
            if (ec) {
                co_return;
            }
            auto const connection{ ++m_connections };
            if (auto const* name{ ::SSL_get_servername(stream.native_handle(), TLSEXT_NAMETYPE_host_name) }) {
                m_sni.store(std::string_view{ name } == "localhost");
            }
            boost::beast::flat_buffer buffer;
            for (;;) {
                HttpRequest request;
                co_await http::async_read(stream, buffer, request, net::redirect_error(net::use_awaitable, ec));
                if (ec) {
                    co_return;
                }
                if (request.target() == "/hold") {
                    m_hold_seen.store(true);
                    std::array<char, 1> byte{};
                    co_await stream.async_read_some(net::buffer(byte), net::redirect_error(net::use_awaitable, ec));
                    m_peer_closed.store(static_cast<bool>(ec));
                    co_return;
                }
                if (request.method() == http::verb::connect) {
                    http::response<http::empty_body> established{ http::status::ok, 11 };
                    co_await http::async_write(stream, established, net::redirect_error(net::use_awaitable, ec));
                    if (ec) {
                        co_return;
                    }
                    co_await net::async_write(stream, net::buffer("ready", 5), net::redirect_error(net::use_awaitable, ec));
                    std::array<char, 4096> bytes{};
                    while (!ec) {
                        auto const size{ co_await stream.async_read_some(net::buffer(bytes), net::redirect_error(net::use_awaitable, ec)) };
                        if (ec) {
                            co_return;
                        }
                        co_await net::async_write(stream, net::buffer(bytes.data(), size), net::redirect_error(net::use_awaitable, ec));
                    }
                    co_return;
                }
                HttpResponse response{ http::status::ok, 11 };
                response.set(http::field::content_type, "text/plain");
                response.set("X-Connection", std::to_string(connection));
                response.body() = request.body().empty() ? "secure" : request.body();
                response.keep_alive(request.keep_alive());
                response.prepare_payload();
                co_await http::async_write(stream, response, net::redirect_error(net::use_awaitable, ec));
                if (ec || !response.keep_alive()) {
                    co_await stream.async_shutdown(net::redirect_error(net::use_awaitable, ec));
                    co_return;
                }
            }
        }
    };

} // namespace test_support
