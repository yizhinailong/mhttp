/**
 * @file tls.cppm
 * @brief Build shared TLS contexts with certificate and peer identity verification.
 */
module;

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/system/system_error.hpp>
#ifdef _WIN32
    #include <wincrypt.h>
    #include <windows.h>
#endif

export module mhttp:tls;

import std;
import :config;

namespace mhttp {
    class HttpStream;
}

namespace mhttp::detail {

    /**
     * @brief Return the last OpenSSL error, with a fallback when its queue is empty.
     */
    auto tls_error() -> boost::system::error_code {
        auto const error{ ::ERR_get_error() };
        return error ? boost::system::error_code{ static_cast<int>(error), net::error::get_ssl_category() } : boost::system::error_code{ net::error::invalid_argument };
    }

    /**
     * @brief Load the operating system's trusted roots into an OpenSSL context.
     */
    auto load_system_roots(net::ssl::context& context) -> Result<void> {
#ifdef _WIN32
        auto const store{ ::CertOpenSystemStoreW(0, L"ROOT") };
        if (!store) {
            return std::unexpected{
                boost::system::error_code{ static_cast<int>(::GetLastError()), boost::system::system_category() }
            };
        }
        auto* destination{ ::SSL_CTX_get_cert_store(context.native_handle()) };
        PCCERT_CONTEXT certificate{};
        while ((certificate = ::CertEnumCertificatesInStore(store, certificate)) != nullptr) {
            auto const* bytes{ certificate->pbCertEncoded };
            auto* decoded{ ::d2i_X509(nullptr, &bytes, certificate->cbCertEncoded) };
            if (decoded) {
                // Duplicate roots and roots rejected by this OpenSSL build are harmless here.
                ::X509_STORE_add_cert(destination, decoded);
                ::X509_free(decoded);
                ::ERR_clear_error();
            }
        }
        ::CertCloseStore(store, 0);
#else
        boost::system::error_code ec;
        context.set_default_verify_paths(ec);
        if (ec) {
            return std::unexpected{ ec };
        }
#endif
        return {};
    }

    /**
     * @brief Require TLS 1.2 or later and disable TLS-level compression.
     */
    auto configure_tls(net::ssl::context& context) -> Result<void> {
        if (::SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1) {
            return std::unexpected{ tls_error() };
        }
        boost::system::error_code ec;
        context.set_options(net::ssl::context::default_workarounds | net::ssl::context::no_compression, ec);
        if (ec) {
            return std::unexpected{ ec };
        }
        return {};
    }

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Trust settings used to construct an immutable client TLS context.
     */
    struct TlsClientConfig {
        bool verify_peer{ true };   ///< Verify both the certificate chain and target host.
        bool use_system_ca{ true }; ///< Load native Windows roots or OpenSSL default CA paths.
        std::string ca_file;        ///< Additional PEM CA bundle file; empty skips it.
        std::string ca_pem;         ///< Additional PEM CA certificates; empty skips them.
    };

    /**
     * @brief Shared client trust configuration; safe to reuse across connections and threads.
     */
    class TlsClientContext {
    private:
        net::ssl::context m_context{ net::ssl::context::tls_client }; ///< Immutable after Create().
        bool m_verify_peer{ true };                                   ///< Per-stream identity check.

        friend class HttpStream;

        TlsClientContext() = default;

    public:
        /**
         * @brief Validate trust sources and create a reusable context.
         * @param config Trust configuration; defaults verify against system roots.
         * @return Shared context or a configuration/OpenSSL error.
         */
        [[nodiscard]] static auto Create(
            TlsClientConfig const& config = {}
        ) -> Result<std::shared_ptr<TlsClientContext>> {
            try {
                auto context{ std::shared_ptr<TlsClientContext>{ new TlsClientContext } };
                if (auto result{ detail::configure_tls(context->m_context) }; !result) {
                    return std::unexpected{ result.error() };
                }
                context->m_verify_peer = config.verify_peer;
                boost::system::error_code ec;
                context->m_context.set_verify_mode(config.verify_peer ? net::ssl::verify_peer : net::ssl::verify_none, ec);
                if (ec) {
                    return std::unexpected{ ec };
                }
                if (config.use_system_ca && config.verify_peer) {
                    if (auto result{ detail::load_system_roots(context->m_context) }; !result) {
                        return std::unexpected{ result.error() };
                    }
                }
                if (!config.ca_file.empty()) {
                    context->m_context.load_verify_file(config.ca_file, ec);
                    if (ec) {
                        return std::unexpected{ ec };
                    }
                }
                if (!config.ca_pem.empty()) {
                    context->m_context.add_certificate_authority(net::buffer(config.ca_pem), ec);
                    if (ec) {
                        return std::unexpected{ ec };
                    }
                }
                return context;
            } catch (boost::system::system_error const& error) {
                return std::unexpected{ error.code() };
            }
        }
    };

    /**
     * @brief Server identity supplied as PEM files or PEM text, one source per field.
     */
    struct TlsServerConfig {
        std::string certificate_chain_file; ///< PEM leaf certificate followed by intermediates.
        std::string private_key_file;       ///< Unencrypted PEM private key file.
        std::string certificate_chain_pem;  ///< In-memory alternative to certificate_chain_file.
        std::string private_key_pem;        ///< In-memory alternative to private_key_file.
    };

    /**
     * @brief Validated, shared server certificate and key; immutable after construction.
     */
    class TlsServerContext {
    private:
        net::ssl::context m_context{ net::ssl::context::tls_server }; ///< Server identity.

        friend class HttpStream;

        TlsServerContext() = default;

    public:
        /**
         * @brief Load a certificate chain and matching private key before accepting clients.
         * @return Shared context or an input, file, PEM, or key-mismatch error.
         */
        [[nodiscard]] static auto Create(
            TlsServerConfig const& config
        ) -> Result<std::shared_ptr<TlsServerContext>> {
            if (config.certificate_chain_file.empty() == config.certificate_chain_pem.empty() || config.private_key_file.empty() == config.private_key_pem.empty()) {
                return std::unexpected{ boost::system::error_code{ net::error::invalid_argument } };
            }
            try {
                auto context{ std::shared_ptr<TlsServerContext>{ new TlsServerContext } };
                if (auto result{ detail::configure_tls(context->m_context) }; !result) {
                    return std::unexpected{ result.error() };
                }
                boost::system::error_code ec;
                if (!config.certificate_chain_file.empty()) {
                    context->m_context.use_certificate_chain_file(config.certificate_chain_file, ec);
                } else {
                    context->m_context.use_certificate_chain(net::buffer(config.certificate_chain_pem), ec);
                }
                if (ec) {
                    return std::unexpected{ ec };
                }
                if (!config.private_key_file.empty()) {
                    context->m_context.use_private_key_file(config.private_key_file, net::ssl::context::pem, ec);
                } else {
                    context->m_context.use_private_key(net::buffer(config.private_key_pem), net::ssl::context::pem, ec);
                }
                if (ec) {
                    return std::unexpected{ ec };
                }
                if (::SSL_CTX_check_private_key(context->m_context.native_handle()) != 1) {
                    return std::unexpected{ detail::tls_error() };
                }
                return context;
            } catch (boost::system::system_error const& error) {
                return std::unexpected{ error.code() };
            }
        }
    };

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Lazily prepare the shared default client trust store once.
     */
    auto default_tls_client_context() -> Result<std::shared_ptr<TlsClientContext>> {
        static auto const s_context{ TlsClientContext::Create() };
        return s_context;
    }

} // namespace mhttp::detail
