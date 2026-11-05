/**
 * @file client_request.cppm
 * @brief Provide the client-side request: method, target, headers, and an outgoing body.
 */
module;

#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:client_request;

import std;
import :config;
import :error;
import :http_date;
import :query_params;
import :form_data;
import :headers;
import :http_ranges;
import :mime_types;
import :body_writer;

export namespace mhttp::client {

    /**
     * @brief One outgoing HTTP request, reusable across sends.
     *
     * New requests carry HTTP/1.1, keep-alive, an Accept field for any media type, and a
     * User-Agent; the client fills in Host when the request has none. Body setters replace any
     * earlier body and derive framing when the request is sent.
     */
    class Request {
    private:
        std::unique_ptr<detail::BodyWriter<true>> m_impl; ///< Message and body source.

    public:
        /**
         * @brief Create a request.
         * @param method Method.
         * @param target Origin-form target, e.g. "/path?x=1".
         * @param headers Fields that replace the defaults of the same name.
         */
        Request(Verb method, std::string_view target, Headers const& headers = {});

        /**
         * @brief Create a request with query parameters appended to the path.
         * @param method Method.
         * @param path Path, optionally with a query already.
         * @param params Parameters to encode and append.
         * @param headers Fields that replace the defaults of the same name.
         */
        Request(
            Verb method,
            std::string_view path,
            QueryParams const& params,
            Headers const& headers = {}
        );

        Request(Request&& other) noexcept;
        auto operator=(Request&& other) noexcept -> Request&;
        ~Request();

        [[nodiscard]] auto Method() const -> Verb;
        auto Method(Verb method) -> void;
        [[nodiscard]] auto Target() const -> std::string_view;
        auto Target(std::string_view target) -> void;

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

        auto Set(Field name, std::string_view value) -> void;
        auto Set(std::string_view name, std::string_view value) -> void;
        auto Insert(Field name, std::string_view value) -> void;
        auto Insert(std::string_view name, std::string_view value) -> void;
        auto Erase(Field name) -> void;
        auto Erase(std::string_view name) -> void;
        [[nodiscard]] auto Has(Field name) const -> bool;
        [[nodiscard]] auto Has(std::string_view name) const -> bool;
        [[nodiscard]] auto Count(Field name) const -> std::size_t;
        [[nodiscard]] auto Count(std::string_view name) const -> std::size_t;

        /**
         * @brief Replace fields name by name with the ones in `headers`.
         * @param headers Fields to apply.
         */
        auto Replace(Headers const& headers) -> void;

        /**
         * @brief Borrow every request header.
         * @return View whose writes land on this request.
         */
        [[nodiscard]] auto Base() const -> Headers;

        /**
         * @brief Announce a body length explicitly, e.g. for a body streamed by the caller.
         * @param length Content-Length value.
         */
        auto ContentLength(std::uint64_t length) -> void;

        [[nodiscard]] auto KeepAlive() const -> bool;
        auto KeepAlive(bool value) -> void;

        /**
         * @brief Send a text body.
         * @param data Body bytes.
         * @param content_type Content-Type value.
         */
        auto SetStringBody(std::string data, std::string_view content_type) -> void;

        /**
         * @brief Send a JSON body (application/json; charset=utf-8).
         * @param data Document; serialized immediately.
         * @throws nlohmann::json::type_error If a string holds invalid UTF-8.
         */
        auto SetJsonBody(Json const& data) -> void;

        /**
         * @brief Send an application/x-www-form-urlencoded body.
         * @param data Parameters.
         */
        auto SetQueryParamsBody(QueryParams const& data) -> void;

        /**
         * @brief Send a multipart/form-data body; file parts stream from disk when sent.
         * @param fields Parts; files referenced by `file_path` stay the caller's.
         */
        auto SetFormDataBody(std::vector<FormData::Field> fields) -> void;

        /**
         * @brief Send a file as the body, typed by its extension.
         * @param path File to send.
         * @return no_such_file_or_directory when the file cannot be opened.
         */
        auto SetFileBody(std::filesystem::path const& path) -> Result<void>;

        friend auto get_impl(Request& request) -> detail::BodyWriter<true>& {
            return *request.m_impl;
        }

        friend auto get_impl(Request const& request) -> detail::BodyWriter<true> const& {
            return *request.m_impl;
        }
    };

} // namespace mhttp::client

namespace mhttp::client {

    Request::Request(
        Verb method,
        std::string_view target,
        Headers const& headers
    ) : m_impl{ std::make_unique<detail::BodyWriter<true>>() } {
        auto& base{ m_impl->Base() };
        base.method(method);
        base.target(target);
        base.version(11);
        base.set(Field::user_agent, BOOST_BEAST_VERSION_STRING);
        base.set(Field::accept, "*/*");
        m_impl->KeepAlive(true);
        m_impl->ReplaceFields(headers);
    }

    Request::Request(
        Verb method,
        std::string_view path,
        QueryParams const& params,
        Headers const& headers
    ) : Request{ method, path, headers } {
        if (!params.Empty()) {
            std::string target{ path };
            target += path.contains('?') ? '&' : '?';
            target += params.Encoded();
            m_impl->Base().target(target);
        }
    }

    Request::Request(Request&& other) noexcept = default;

    auto Request::operator=(Request&& other) noexcept -> Request& = default;

    Request::~Request() = default;

    auto Request::Method() const -> Verb {
        return m_impl->Base().method();
    }

    auto Request::Method(Verb method) -> void {
        m_impl->Base().method(method);
    }

    auto Request::Target() const -> std::string_view {
        return detail::to_sv(m_impl->Base().target());
    }

    auto Request::Target(std::string_view target) -> void {
        m_impl->Base().target(target);
    }

    auto Request::operator[](Field name) const -> std::string_view {
        return detail::to_sv(m_impl->Base()[name]);
    }

    auto Request::operator[](std::string_view name) const -> std::string_view {
        return detail::to_sv(m_impl->Base()[name]);
    }

    auto Request::At(Field name) const -> std::string_view {
        return detail::to_sv(m_impl->Base().at(name));
    }

    auto Request::At(std::string_view name) const -> std::string_view {
        return detail::to_sv(m_impl->Base().at(name));
    }

    auto Request::Set(Field name, std::string_view value) -> void {
        m_impl->Base().set(name, value);
    }

    auto Request::Set(std::string_view name, std::string_view value) -> void {
        m_impl->Base().set(name, value);
    }

    auto Request::Insert(Field name, std::string_view value) -> void {
        m_impl->Base().insert(name, value);
    }

    auto Request::Insert(std::string_view name, std::string_view value) -> void {
        m_impl->Base().insert(name, value);
    }

    auto Request::Erase(Field name) -> void {
        m_impl->Base().erase(name);
    }

    auto Request::Erase(std::string_view name) -> void {
        m_impl->Base().erase(name);
    }

    auto Request::Has(Field name) const -> bool {
        return m_impl->Base().find(name) != m_impl->Base().end();
    }

    auto Request::Has(std::string_view name) const -> bool {
        return m_impl->Base().find(name) != m_impl->Base().end();
    }

    auto Request::Count(Field name) const -> std::size_t {
        return m_impl->Base().count(name);
    }

    auto Request::Count(std::string_view name) const -> std::size_t {
        return m_impl->Base().count(name);
    }

    auto Request::Replace(Headers const& headers) -> void {
        m_impl->ReplaceFields(headers);
    }

    auto Request::Base() const -> Headers {
        return Headers::Borrow(m_impl->Base());
    }

    auto Request::ContentLength(std::uint64_t length) -> void {
        m_impl->ContentLength(length);
    }

    auto Request::KeepAlive() const -> bool {
        return m_impl->KeepAlive();
    }

    auto Request::KeepAlive(bool value) -> void {
        m_impl->KeepAlive(value);
    }

    auto Request::SetStringBody(std::string data, std::string_view content_type) -> void {
        m_impl->SetString(std::move(data), content_type);
    }

    auto Request::SetJsonBody(Json const& data) -> void {
        m_impl->SetJson(data);
    }

    auto Request::SetQueryParamsBody(QueryParams const& data) -> void {
        m_impl->SetQueryParams(data);
    }

    auto Request::SetFormDataBody(std::vector<FormData::Field> fields) -> void {
        FormData value;
        value.boundary = detail::generate_boundary();
        value.fields = std::move(fields);
        // Files referenced by a request belong to the caller.
        value.params.remove_uploaded_files = false;
        m_impl->SetFormData(std::move(value));
    }

    auto Request::SetFileBody(std::filesystem::path const& path) -> Result<void> {
        std::ifstream file{ path, std::ios::in | std::ios::binary };
        if (!file.is_open()) {
            return std::unexpected{ boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory) };
        }
        m_impl->SetFile(std::move(file), std::string{ detail::get_mime_type(path.extension().string()) }, detail::HttpRanges{});
        return {};
    }

} // namespace mhttp::client
