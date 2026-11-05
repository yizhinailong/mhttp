/**
 * @file request.cppm
 * @brief Provide the server-side request: header access, routing data, and lazy body reads.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

export module mhttp:request;

import std;
import :config;
import :error;
import :strings;
import :query_params;
import :form_data;
import :headers;
import :body_state;
import :connection;
import :body_reader;
import :trusted_proxies;

namespace mhttp::detail {

    /**
     * @brief Split a raw path on literal '/' without trimming or merging separators.
     *
     * Segment boundaries are fixed before decoding, so "%2F" never splits. A trailing '/'
     * yields an empty segment; "/" is a single empty segment.
     *
     * @param path Raw path.
     * @return Views into `path`.
     */
    auto split_path_segments(std::string_view path) -> std::vector<std::string_view> {
        std::vector<std::string_view> out;
        if (path.empty()) {
            return out;
        }
        if (path.front() == '/') {
            path.remove_prefix(1);
        }
        if (path.empty()) {
            out.emplace_back();
            return out;
        }
        std::size_t begin{ 0 };
        for (;;) {
            auto const slash{ path.find('/', begin) };
            if (slash == std::string_view::npos) {
                out.push_back(path.substr(begin));
                break;
            }
            out.push_back(path.substr(begin, slash - begin));
            begin = slash + 1;
            if (begin == path.size()) {
                out.emplace_back();
                break;
            }
        }
        return out;
    }

    /**
     * @brief Split on literal '/' and percent-decode each segment separately.
     * @param path Raw path.
     * @return Decoded segments; a segment may contain '/' that came from "%2F".
     */
    auto decode_path_segments(std::string_view path) -> std::vector<std::string> {
        std::vector<std::string> out;
        for (auto const segment : split_path_segments(path)) {
            out.push_back(segment.find('%') == std::string_view::npos ? std::string{ segment } : url::url_decode(segment));
        }
        return out;
    }

} // namespace mhttp::detail

namespace mhttp {

    class RequestImpl;

} // namespace mhttp

export namespace mhttp {

    /**
     * @brief Per-request storage for values passed between middleware and handlers.
     *
     * Keys are the decayed type plus an optional tag; values are copied in and out.
     */
    class RequestData {
    private:
        mutable std::mutex m_mutex;                     ///< Guards m_values.
        detail::StringMap<std::any> m_values;           ///< Stored values keyed by type and tag.

    public:
        /**
         * @brief Store a value under its type.
         * @tparam T Value type; the key uses std::decay_t<T>.
         * @param value Value to store.
         */
        template <typename T>
        auto Store(T&& value) -> void {
            Store("", std::forward<T>(value));
        }

        /**
         * @brief Store a value under its type and a tag.
         * @tparam T Value type; the key uses std::decay_t<T>.
         * @param tag Distinguishes several values of the same type.
         * @param value Value to store.
         */
        template <typename T>
        auto Store(std::string_view tag, T&& value) -> void {
            std::lock_guard lock{ m_mutex };
            m_values[makeKey<T>(tag)] = std::any{ std::forward<T>(value) };
        }

        /**
         * @brief Copy a stored value out.
         * @tparam T Value type.
         * @param tag Tag used when storing.
         * @return Copy of the value, or std::nullopt when absent.
         */
        template <typename T>
        [[nodiscard]] auto Fetch(
            std::string_view tag = ""
        ) const -> std::optional<std::decay_t<T>> {
            std::lock_guard lock{ m_mutex };
            auto const it{ m_values.find(makeKey<T>(tag)) };
            if (it == m_values.end()) {
                return std::nullopt;
            }
            return std::any_cast<std::decay_t<T>>(it->second);
        }

        /**
         * @brief Report whether a value is stored; use Fetch() to read it in one locked step.
         * @tparam T Value type.
         * @param tag Tag used when storing.
         * @return True when present.
         */
        template <typename T>
        [[nodiscard]] auto Has(std::string_view tag = "") const -> bool {
            std::lock_guard lock{ m_mutex };
            return m_values.contains(makeKey<T>(tag));
        }

        /**
         * @brief Remove a stored value.
         * @tparam T Value type.
         * @param tag Tag used when storing.
         */
        template <typename T>
        auto Erase(std::string_view tag = "") -> void {
            std::lock_guard lock{ m_mutex };
            m_values.erase(makeKey<T>(tag));
        }

    private:
        template <typename T>
        static auto makeKey(std::string_view tag) -> std::string {
            std::string key{ typeid(std::decay_t<T>).name() };
            if (!tag.empty()) {
                key += ':';
                key += tag;
            }
            return key;
        }
    };

    /**
     * @brief One server-side HTTP request.
     *
     * Only use it on the connection strand that runs the handler. The body is read lazily:
     * regular routes are materialized before the handler runs (use As*()), lazy routes let the
     * handler stream with ReadSomeRaw() or materialize with Read*().
     */
    class Request {
    private:
        std::unique_ptr<RequestImpl> m_impl; ///< Request state.

    public:
        /**
         * @brief Wrap request state; created by the server.
         * @param impl Request state.
         */
        explicit Request(std::unique_ptr<RequestImpl> impl);

        Request(Request&& other) noexcept;
        auto operator=(Request&& other) noexcept -> Request&;
        ~Request();

        [[nodiscard]] auto Method() const -> Verb;
        [[nodiscard]] auto MethodString() const -> std::string_view;
        [[nodiscard]] auto Target() const -> std::string_view;

        /**
         * @brief Return the HTTP version.
         * @return 11 for HTTP/1.1, 10 for HTTP/1.0.
         */
        [[nodiscard]] auto Version() const -> unsigned;

        /**
         * @brief Borrow every request header.
         * @return View whose writes land on this request.
         */
        [[nodiscard]] auto Base() const -> Headers;

        [[nodiscard]] auto operator[](Field name) const -> std::string_view;
        [[nodiscard]] auto operator[](std::string_view name) const -> std::string_view;

        /**
         * @brief Return a header value.
         * @param name Field to look up.
         * @return Value.
         * @throws std::out_of_range If the field is absent.
         */
        [[nodiscard]] auto At(Field name) const -> std::string_view;
        [[nodiscard]] auto At(std::string_view name) const -> std::string_view;

        [[nodiscard]] auto Has(Field name) const -> bool;
        [[nodiscard]] auto Has(std::string_view name) const -> bool;
        [[nodiscard]] auto Count(Field name) const -> std::size_t;
        [[nodiscard]] auto Count(std::string_view name) const -> std::size_t;

        /**
         * @brief Return the raw path (target before '?'), percent-encoding kept.
         * @return Path view valid for the request lifetime.
         */
        [[nodiscard]] auto Path() const -> std::string_view;

        /**
         * @brief Fully percent-decode the path; for display only, since "%2F" becomes '/'.
         * @return Decoded path.
         */
        [[nodiscard]] auto DecodedPath() const -> std::string;

        /**
         * @brief Return the path split on literal '/' and decoded per segment.
         * @note Segments may still contain '/', '\\', or NUL; validate before using them as paths.
         * @return Segments valid for the request lifetime.
         */
        [[nodiscard]] auto DecodedSegments() const -> std::span<std::string const>;

        /**
         * @brief Return the decoded query string parameters.
         * @return Parameters parsed from the target.
         */
        [[nodiscard]] auto Query() const -> QueryParams const&;

        /**
         * @brief Return the client address, honoring X-Forwarded-For only from trusted proxies.
         * @return Rightmost untrusted X-Forwarded-For hop, or the TCP peer address.
         */
        [[nodiscard]] auto GetClientIp() const -> net::ip::address;

        [[nodiscard]] auto LocalEndpoint() const -> tcp::endpoint const&;
        [[nodiscard]] auto RemoteEndpoint() const -> tcp::endpoint const&;

        [[nodiscard]] auto Data() -> RequestData&;
        [[nodiscard]] auto Data() const -> RequestData const&;

        /**
         * @brief Borrow the materialized body as text.
         * @return Body; empty for body-less requests.
         * @throws std::bad_variant_access If the body is unread or of another type.
         */
        [[nodiscard]] auto AsString() const -> std::string const&;
        [[nodiscard]] auto AsJson() const -> Json const&;
        [[nodiscard]] auto AsFormData() const -> FormData const&;
        [[nodiscard]] auto AsQueryParams() const -> QueryParams const&;

        /**
         * @brief Return the materialized body type.
         * @return BodyType::None until the body was read.
         */
        [[nodiscard]] auto Type() const -> BodyType;

        /**
         * @brief Read the rest of the body and move it out as text.
         * @return Body text.
         * @throws boost::system::system_error On read failure.
         */
        auto ReadString() -> net::awaitable<std::string>;
        auto ReadJson() -> net::awaitable<Json>;
        auto ReadFormData() -> net::awaitable<FormData>;
        auto ReadQueryParams() -> net::awaitable<QueryParams>;

        /**
         * @brief Read the rest of the body and materialize it by Content-Type (see As*()).
         * @return Error code.
         */
        auto ReadBody() -> net::awaitable<boost::system::error_code>;

        /**
         * @brief Stream raw body bytes.
         * @param buffer Destination.
         * @param ec Receives read errors.
         * @return Bytes read; 0 without error means the body is complete.
         */
        auto ReadSomeRaw(
            net::mutable_buffer buffer,
            boost::system::error_code& ec
        ) -> net::awaitable<std::size_t>;

        /**
         * @brief Stream raw body bytes.
         * @param buffer Destination.
         * @return Bytes read; 0 means the body is complete.
         * @throws boost::system::system_error On read failure.
         */
        auto ReadSomeRaw(net::mutable_buffer buffer) -> net::awaitable<std::size_t>;

        /**
         * @brief Report whether the body was fully read.
         * @return True once every body byte was consumed or materialized.
         */
        [[nodiscard]] auto IsBodyDone() const -> bool;

        /**
         * @brief Return a path parameter matched by ":name", "{name:regex}", or "*".
         * @note Values are percent-decoded per segment; validate before using them as file names.
         * @tparam T std::string_view, std::string, or an arithmetic type.
         * @param key Parameter name; "*" for the wildcard remainder.
         * @return Value; empty view when absent.
         * @throws std::runtime_error If conversion to an arithmetic type fails.
         */
        template <typename T = std::string_view>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, std::string> || std::same_as<T, std::string_view>
        [[nodiscard]] auto PathParam(std::string_view key) const -> T {
            auto const value{ pathParamRaw(key) };
            if constexpr (std::same_as<T, std::string_view>) {
                return value;
            } else if constexpr (std::same_as<T, std::string>) {
                return std::string{ value };
            } else {
                return detail::from_chars_strict<T>(value);
            }
        }

        friend auto get_impl(Request& request) -> RequestImpl& {
            return *request.m_impl;
        }

        friend auto get_impl(Request const& request) -> RequestImpl const& {
            return *request.m_impl;
        }

    private:
        [[nodiscard]] auto pathParamRaw(std::string_view key) const -> std::string_view;
    };

} // namespace mhttp

namespace mhttp {

    /**
     * @brief Request state behind mhttp::Request: parsed target, routing data, and body reader.
     */
    class RequestImpl : public detail::BodyReader<true> {
    private:
        std::string m_raw_path;                                   ///< Target before '?'.
        std::vector<std::string> m_decoded_segments;              ///< Per-segment decoded path.
        QueryParams m_query;                                      ///< Decoded query string.
        tcp::endpoint m_local_endpoint;                           ///< Server side of the connection.
        tcp::endpoint m_remote_endpoint;                          ///< Client side of the connection.
        std::shared_ptr<detail::TrustedProxies const> m_trusted; ///< Trusted proxies snapshot; null = none.
        detail::StringMap<std::string> m_path_params;             ///< Parameters from route matching.
        RequestData m_data;                                       ///< User data.

    public:
        /**
         * @brief Build request state from a parsed header.
         * @param connection Connection supplying the body.
         * @param header_parser Parser after async_read_header.
         * @param local Server endpoint.
         * @param remote Client endpoint.
         * @param trusted Trusted proxies snapshot; may be null.
         * @param form_params Limits for automatic multipart parsing.
         */
        RequestImpl(
            std::shared_ptr<detail::Connection> connection,
            HeaderParser&& header_parser,
            tcp::endpoint const& local,
            tcp::endpoint const& remote,
            std::shared_ptr<detail::TrustedProxies const> trusted,
            FormData::Param form_params
        ) : BodyReader{ std::move(connection), std::move(header_parser) }, m_local_endpoint{ local }, m_remote_endpoint{ remote }, m_trusted{ std::move(trusted) } {
            auto const target{ detail::to_sv(Message().target()) };
            auto const question{ target.find('?') };
            m_raw_path = std::string{ target.substr(0, question) };
            m_decoded_segments = detail::decode_path_segments(m_raw_path);
            if (question != std::string_view::npos) {
                m_query.Decode(target.substr(question + 1));
            }
            SetFormDataParams(std::move(form_params));
        }

        /**
         * @brief Build a Request around new state.
         * @return Request.
         */
        static auto Make(
            std::shared_ptr<detail::Connection> connection,
            HeaderParser&& header_parser,
            tcp::endpoint const& local,
            tcp::endpoint const& remote,
            std::shared_ptr<detail::TrustedProxies const> trusted,
            FormData::Param form_params
        ) -> Request {
            return Request{ std::make_unique<RequestImpl>(std::move(connection), std::move(header_parser), local, remote, std::move(trusted), std::move(form_params)) };
        }

        [[nodiscard]] auto Path() const -> std::string_view {
            return m_raw_path;
        }

        [[nodiscard]] auto DecodedSegments() const -> std::span<std::string const> {
            return m_decoded_segments;
        }

        [[nodiscard]] auto Query() const -> QueryParams const& {
            return m_query;
        }

        [[nodiscard]] auto LocalEndpoint() const -> tcp::endpoint const& {
            return m_local_endpoint;
        }

        [[nodiscard]] auto RemoteEndpoint() const -> tcp::endpoint const& {
            return m_remote_endpoint;
        }

        [[nodiscard]] auto Data() -> RequestData& {
            return m_data;
        }

        [[nodiscard]] auto Data() const -> RequestData const& {
            return m_data;
        }

        [[nodiscard]] auto PathParam(std::string_view key) const -> std::string_view {
            auto const it{ m_path_params.find(key) };
            return it == m_path_params.end() ? std::string_view{} : std::string_view{ it->second };
        }

        auto SetPathParams(detail::StringMap<std::string> params) -> void {
            m_path_params = std::move(params);
        }

        /**
         * @brief Resolve the client address (see Request::GetClientIp()).
         * @return Client address.
         */
        [[nodiscard]] auto GetClientIp() const -> net::ip::address {
            auto const peer{ m_remote_endpoint.address() };
            // X-Forwarded-For is client-controlled; only a trusted direct peer may vouch for it.
            if (!m_trusted || !m_trusted->Contains(peer)) {
                return peer;
            }
            auto const it{ Message().find("X-Forwarded-For") };
            if (it == Message().end()) {
                return peer;
            }
            // Each proxy appends, so the rightmost hops are the closest ones. Skip trusted
            // hops from the right; the leftmost entry is trivially spoofable.
            auto const hops{ detail::split(detail::to_sv(it->value()), ",") };
            for (auto hop{ hops.rbegin() }; hop != hops.rend(); ++hop) {
                boost::system::error_code ec;
                auto const address{ net::ip::make_address(*hop, ec) };
                if (ec) {
                    continue;
                }
                if (!m_trusted->Contains(address)) {
                    return address;
                }
            }
            return peer;
        }
    };

    Request::Request(std::unique_ptr<RequestImpl> impl) : m_impl{ std::move(impl) } {}

    Request::Request(Request&& other) noexcept = default;

    auto Request::operator=(Request&& other) noexcept -> Request& = default;

    Request::~Request() = default;

    auto Request::Method() const -> Verb {
        return m_impl->Message().method();
    }

    auto Request::MethodString() const -> std::string_view {
        return detail::to_sv(m_impl->Message().method_string());
    }

    auto Request::Target() const -> std::string_view {
        return detail::to_sv(m_impl->Message().target());
    }

    auto Request::Version() const -> unsigned {
        return m_impl->Message().version();
    }

    auto Request::Base() const -> Headers {
        return Headers::Borrow(m_impl->Message().base());
    }

    auto Request::operator[](Field name) const -> std::string_view {
        return detail::to_sv(m_impl->Message()[name]);
    }

    auto Request::operator[](std::string_view name) const -> std::string_view {
        return detail::to_sv(m_impl->Message()[name]);
    }

    auto Request::At(Field name) const -> std::string_view {
        return detail::to_sv(m_impl->Message().at(name));
    }

    auto Request::At(std::string_view name) const -> std::string_view {
        return detail::to_sv(m_impl->Message().at(name));
    }

    auto Request::Has(Field name) const -> bool {
        return m_impl->Message().find(name) != m_impl->Message().end();
    }

    auto Request::Has(std::string_view name) const -> bool {
        return m_impl->Message().find(name) != m_impl->Message().end();
    }

    auto Request::Count(Field name) const -> std::size_t {
        return m_impl->Message().count(name);
    }

    auto Request::Count(std::string_view name) const -> std::size_t {
        return m_impl->Message().count(name);
    }

    auto Request::Path() const -> std::string_view {
        return m_impl->Path();
    }

    auto Request::DecodedPath() const -> std::string {
        return url::url_decode(m_impl->Path());
    }

    auto Request::DecodedSegments() const -> std::span<std::string const> {
        return m_impl->DecodedSegments();
    }

    auto Request::Query() const -> QueryParams const& {
        return m_impl->Query();
    }

    auto Request::GetClientIp() const -> net::ip::address {
        return m_impl->GetClientIp();
    }

    auto Request::LocalEndpoint() const -> tcp::endpoint const& {
        return m_impl->LocalEndpoint();
    }

    auto Request::RemoteEndpoint() const -> tcp::endpoint const& {
        return m_impl->RemoteEndpoint();
    }

    auto Request::Data() -> RequestData& {
        return m_impl->Data();
    }

    auto Request::Data() const -> RequestData const& {
        return m_impl->Data();
    }

    auto Request::AsString() const -> std::string const& {
        return m_impl->State().As<std::string>();
    }

    auto Request::AsJson() const -> Json const& {
        return m_impl->State().As<Json>();
    }

    auto Request::AsFormData() const -> FormData const& {
        return m_impl->State().As<FormData>();
    }

    auto Request::AsQueryParams() const -> QueryParams const& {
        return m_impl->State().As<QueryParams>();
    }

    auto Request::Type() const -> BodyType {
        return m_impl->State().Type();
    }

    namespace detail {

        /**
         * @brief Unwrap a read result or throw its error.
         * @tparam T Value type.
         * @param result Read result.
         * @return Moved value.
         * @throws boost::system::system_error If `result` holds an error.
         */
        template <typename T>
        auto value_or_throw(Result<T>&& result) -> T {
            if (!result) {
                throw boost::system::system_error{ result.error() };
            }
            return std::move(*result);
        }

    } // namespace detail

    auto Request::ReadString() -> net::awaitable<std::string> {
        co_return detail::value_or_throw(co_await m_impl->ReadString());
    }

    auto Request::ReadJson() -> net::awaitable<Json> {
        co_return detail::value_or_throw(co_await m_impl->ReadJson());
    }

    auto Request::ReadFormData() -> net::awaitable<FormData> {
        co_return detail::value_or_throw(co_await m_impl->ReadFormData());
    }

    auto Request::ReadQueryParams() -> net::awaitable<QueryParams> {
        co_return detail::value_or_throw(co_await m_impl->ReadQueryParams());
    }

    auto Request::ReadBody() -> net::awaitable<boost::system::error_code> {
        co_return co_await m_impl->ReadBody();
    }

    auto Request::ReadSomeRaw(
        net::mutable_buffer buffer,
        boost::system::error_code& ec
    ) -> net::awaitable<std::size_t> {
        co_return co_await m_impl->ReadSomeRaw(buffer, ec);
    }

    auto Request::ReadSomeRaw(net::mutable_buffer buffer) -> net::awaitable<std::size_t> {
        boost::system::error_code ec;
        auto const bytes{ co_await m_impl->ReadSomeRaw(buffer, ec) };
        if (ec) {
            throw boost::system::system_error{ ec };
        }
        co_return bytes;
    }

    auto Request::IsBodyDone() const -> bool {
        return m_impl->IsBodyDone();
    }

    auto Request::pathParamRaw(std::string_view key) const -> std::string_view {
        return m_impl->PathParam(key);
    }

} // namespace mhttp
