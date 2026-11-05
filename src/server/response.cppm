/**
 * @file response.cppm
 * @brief Provide the server-side response and its streaming, SSE, and NDJSON writers.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

export module mhttp:response;

import std;
import :config;
import :error;
import :strings;
import :http_date;
import :form_data;
import :headers;
import :http_ranges;
import :mime_types;
import :connection;
import :body_writer;

namespace mhttp {

    class ResponseImpl;

} // namespace mhttp

export namespace mhttp {

    /**
     * @brief Low-level streaming response writer created by Response::CreateStreamWriter().
     *
     * Holds the response weakly: once the response is gone (the handler returned), every write
     * fails with net::error::operation_aborted. Use it on the connection strand only and do not
     * overlap calls.
     */
    class StreamWriter {
    public:
        /**
         * @brief How the body is framed.
         */
        enum class Mode {
            Relay,   ///< Keep the framing given in the headers (e.g. an explicit Content-Length).
            Chunked, ///< Use chunked transfer coding.
        };

    private:
        std::weak_ptr<ResponseImpl> m_response; ///< Response being written.
        Mode m_mode{ Mode::Chunked };           ///< Framing chosen by WriteHeader().

    public:
        /**
         * @brief Bind to a response; created by Response::CreateStreamWriter().
         * @param response Response state.
         */
        explicit StreamWriter(std::weak_ptr<ResponseImpl> response) : m_response{ std::move(response) } {}

        /**
         * @brief Send the status line and headers.
         * @param status Response status.
         * @param headers Fields to apply on top of the response's own.
         * @param mode Body framing.
         * @throws boost::system::system_error On failure.
         */
        auto WriteHeader(Status status, Headers const& headers, Mode mode) -> net::awaitable<void>;

        /**
         * @brief Send the status line and headers.
         * @param status Response status.
         * @param headers Fields to apply on top of the response's own.
         * @param mode Body framing.
         * @param ec Receives the error.
         */
        auto WriteHeader(
            Status status,
            Headers const& headers,
            Mode mode,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        /**
         * @brief Send one body chunk.
         * @param data Bytes to send.
         * @param more False for the last chunk.
         * @throws boost::system::system_error On failure.
         */
        auto WriteBody(net::const_buffer data, bool more) -> net::awaitable<void>;

        /**
         * @brief Send one body chunk.
         * @param data Bytes to send.
         * @param more False for the last chunk.
         * @param ec Receives the error.
         */
        auto WriteBody(
            net::const_buffer data,
            bool more,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;
    };

    /**
     * @brief Server-Sent Events writer created by Response::CreateSseWriter().
     */
    class SseWriter {
    private:
        std::shared_ptr<StreamWriter> m_writer; ///< Underlying chunked writer.

    public:
        /**
         * @brief Wrap a stream writer.
         * @param writer Writer of the response.
         */
        explicit SseWriter(std::shared_ptr<StreamWriter> writer) : m_writer{ std::move(writer) } {}

        /**
         * @brief Send the text/event-stream header.
         * @throws boost::system::system_error On failure.
         */
        auto Begin() -> net::awaitable<void> {
            co_await throwing([this](boost::system::error_code& ec) { return Begin(ec); });
        }

        auto Begin(boost::system::error_code& ec) -> net::awaitable<void> {
            Headers headers;
            headers.Set(Field::content_type, "text/event-stream");
            headers.Set(Field::cache_control, "no-cache");
            co_await m_writer->WriteHeader(Status::ok, headers, StreamWriter::Mode::Chunked, ec);
        }

        /**
         * @brief Send one event; multi-line data becomes several "data:" lines.
         * @param data Event data.
         * @param event Event type; empty omits the field.
         * @param id Event id; empty omits the field.
         * @param more False to end the stream after this event.
         * @throws boost::system::system_error On failure.
         */
        auto SendEvent(
            std::string_view data,
            std::string_view event,
            std::string_view id,
            bool more
        ) -> net::awaitable<void> {
            co_await throwing([&](boost::system::error_code& ec) { return SendEvent(data, event, id, more, ec); });
        }

        auto SendEvent(
            std::string_view data,
            std::string_view event,
            std::string_view id,
            bool more,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            std::string message;
            if (!id.empty()) {
                message += std::format("id: {}\n", id);
            }
            if (!event.empty()) {
                message += std::format("event: {}\n", event);
            }
            std::size_t start{ 0 };
            for (;;) {
                auto const newline{ data.find('\n', start) };
                auto line{ data.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start) };
                if (line.ends_with('\r')) {
                    line.remove_suffix(1);
                }
                if (newline == std::string_view::npos) {
                    // A trailing newline does not start another data line.
                    if (start < data.size() || start == 0) {
                        message += std::format("data: {}\n", line);
                    }
                    break;
                }
                message += std::format("data: {}\n", line);
                start = newline + 1;
            }
            message += '\n';
            co_await m_writer->WriteBody(net::buffer(message), more, ec);
        }

        /**
         * @brief Tell the client how long to wait before reconnecting.
         * @param delay Reconnection delay.
         * @param more False to end the stream after this field.
         * @throws boost::system::system_error On failure.
         */
        auto SendRetry(std::chrono::milliseconds delay, bool more) -> net::awaitable<void> {
            co_await throwing([&](boost::system::error_code& ec) { return SendRetry(delay, more, ec); });
        }

        auto SendRetry(
            std::chrono::milliseconds delay,
            bool more,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            auto const message{ std::format("retry: {}\n\n", delay.count()) };
            co_await m_writer->WriteBody(net::buffer(message), more, ec);
        }

        /**
         * @brief Send a comment line, e.g. as a keep-alive ping.
         * @param comment Comment text.
         * @param more False to end the stream after this comment.
         * @throws boost::system::system_error On failure.
         */
        auto SendComment(std::string_view comment, bool more) -> net::awaitable<void> {
            co_await throwing([&](boost::system::error_code& ec) { return SendComment(comment, more, ec); });
        }

        auto SendComment(
            std::string_view comment,
            bool more,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            auto const message{ std::format(": {}\n\n", comment) };
            co_await m_writer->WriteBody(net::buffer(message), more, ec);
        }

    private:
        /**
         * @brief Run an error-code overload and throw its error.
         * @param operation Callable taking an error_code& and returning the awaitable.
         * @throws boost::system::system_error On failure.
         */
        static auto throwing(auto operation) -> net::awaitable<void> {
            boost::system::error_code ec;
            co_await operation(ec);
            if (ec) {
                throw boost::system::system_error{ ec };
            }
        }
    };

    /**
     * @brief Newline-delimited JSON writer created by Response::CreateNdjsonWriter().
     */
    class NdjsonWriter {
    private:
        std::shared_ptr<StreamWriter> m_writer; ///< Underlying chunked writer.

    public:
        /**
         * @brief Wrap a stream writer.
         * @param writer Writer of the response.
         */
        explicit NdjsonWriter(std::shared_ptr<StreamWriter> writer) : m_writer{ std::move(writer) } {}

        /**
         * @brief Send the application/x-ndjson header.
         * @throws boost::system::system_error On failure.
         */
        auto Begin() -> net::awaitable<void> {
            boost::system::error_code ec;
            co_await Begin(ec);
            if (ec) {
                throw boost::system::system_error{ ec };
            }
        }

        auto Begin(boost::system::error_code& ec) -> net::awaitable<void> {
            Headers headers;
            headers.Set(Field::content_type, "application/x-ndjson");
            co_await m_writer->WriteHeader(Status::ok, headers, StreamWriter::Mode::Chunked, ec);
        }

        /**
         * @brief Send one JSON document followed by a newline.
         * @param value Document to send.
         * @param more False to end the stream after this line.
         * @throws boost::system::system_error On failure.
         */
        auto Write(Json const& value, bool more) -> net::awaitable<void> {
            boost::system::error_code ec;
            co_await Write(value, more, ec);
            if (ec) {
                throw boost::system::system_error{ ec };
            }
        }

        auto Write(
            Json const& value,
            bool more,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            auto const line{ value.dump() + "\n" };
            co_await m_writer->WriteBody(net::buffer(line), more, ec);
        }
    };

    /**
     * @brief One server-side HTTP response.
     *
     * Starts as 404 with Server and Date headers; content setters replace status and body.
     * Use it (and its writers) on the connection strand only.
     */
    class Response {
    private:
        std::shared_ptr<ResponseImpl> m_impl; ///< Response state; shared with stream writers weakly.

        explicit Response(std::shared_ptr<ResponseImpl> impl);

        friend class ResponseImpl;

    public:
        Response(Response const&) = delete;
        auto operator=(Response const&) -> Response& = delete;
        ~Response();

        /**
         * @brief Borrow every response header.
         * @return View whose writes land on this response.
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

        [[nodiscard]] auto Result() const -> Status;
        [[nodiscard]] auto ResultInt() const -> unsigned;

        /**
         * @brief Set a status with an empty body.
         * @param status Response status.
         */
        auto SetEmptyContent(Status status) -> void;

        /**
         * @brief Set a status with a small HTML error page.
         * @param status Response status.
         */
        auto SetErrorContent(Status status) -> void;

        /**
         * @brief Set a text body.
         * @param data Body bytes.
         * @param content_type Content-Type value.
         * @param status Response status.
         */
        auto SetStringContent(
            std::string data,
            std::string_view content_type,
            Status status = Status::ok
        ) -> void;

        /**
         * @brief Set a JSON body.
         * @param data Document to send.
         * @param status Response status.
         * @throws nlohmann::json::type_error If a string holds invalid UTF-8.
         */
        auto SetJsonContent(Json const& data, Status status = Status::ok) -> void;

        /**
         * @brief Serve a file with ETag/Last-Modified validation and Range support.
         * @param path File to send.
         * @param request_headers Request headers (Range, If-None-Match, If-Modified-Since).
         */
        auto SetFileContent(
            std::filesystem::path const& path,
            Headers const& request_headers = {}
        ) -> void;

        /**
         * @brief Set a multipart/form-data body; file parts stream from disk and stay the caller's.
         * @param fields Parts to send.
         */
        auto SetFormDataContent(std::vector<FormData::Field> fields) -> void;

        /**
         * @brief Redirect with an empty body.
         * @param url Location value.
         * @param status Redirect status.
         */
        auto SetRedirect(std::string_view url, Status status = Status::moved_permanently) -> void;

        [[nodiscard]] auto CreateSseWriter() -> std::unique_ptr<SseWriter>;
        [[nodiscard]] auto CreateNdjsonWriter() -> std::unique_ptr<NdjsonWriter>;

        /**
         * @brief Return the response's stream writer, creating it on first use.
         * @return Writer that holds this response weakly.
         */
        [[nodiscard]] auto CreateStreamWriter() -> std::shared_ptr<StreamWriter>;

        friend auto get_impl(Response& response) -> ResponseImpl& {
            return *response.m_impl;
        }

        friend auto get_impl(Response const& response) -> ResponseImpl const& {
            return *response.m_impl;
        }
    };

} // namespace mhttp

namespace mhttp {

    /**
     * @brief Response state behind mhttp::Response: message, body source, and stream writer.
     */
    class ResponseImpl : public detail::BodyWriter<false> {
    private:
        std::shared_ptr<StreamWriter> m_stream_writer; ///< Lazily created writer.

    public:
        /**
         * @brief Create a 404 response with Server and Date headers.
         * @param version HTTP version of the request.
         * @param keep_alive Keep-alive preference of the request.
         * @param connection Destination connection.
         */
        ResponseImpl(unsigned version, bool keep_alive, std::shared_ptr<detail::Connection> connection) : BodyWriter{ std::move(connection) } {
            Base().result(Status::not_found);
            Base().version(version);
            Base().set(Field::server, BOOST_BEAST_VERSION_STRING);
            Base().set(Field::date, detail::current_gmt_date());
            KeepAlive(keep_alive);
        }

        /**
         * @brief Build a Response around new state.
         * @param version HTTP version of the request.
         * @param keep_alive Keep-alive preference of the request.
         * @param connection Destination connection.
         * @return Response.
         */
        static auto Create(
            unsigned version,
            bool keep_alive,
            std::shared_ptr<detail::Connection> connection
        ) -> Response {
            return Response{ std::make_shared<ResponseImpl>(version, keep_alive, std::move(connection)) };
        }

        /**
         * @brief Return the stream writer, creating it on first use.
         * @param self Weak handle to this object.
         * @return Shared writer.
         */
        auto GetStreamWriter(std::weak_ptr<ResponseImpl> self) -> std::shared_ptr<StreamWriter> {
            if (!m_stream_writer) {
                m_stream_writer = std::make_shared<StreamWriter>(std::move(self));
            }
            return m_stream_writer;
        }

        auto SetEmptyContent(Status status) -> void {
            if (HeaderSent()) {
                return;
            }
            SetEmpty();
            Base().result(status);
            ContentLength(0);
        }

        auto SetErrorContent(Status status) -> void {
            auto const code{ static_cast<unsigned>(status) };
            auto const reason{ detail::to_sv(http::obsolete_reason(status)) };
            auto content{ std::format(
                "<html>\n<head><title>{0} {1}</title></head>\n<body bgcolor=\"white\">\n<center><h1>{0} {1}</h1></center>\n<hr><center>{2}</center>\n</body>\n</html>",
                code,
                reason,
                detail::to_sv(Base()[Field::server])
            ) };
            SetStringContent(std::move(content), "text/html; charset=utf-8", status);
        }

        auto SetStringContent(
            std::string data,
            std::string_view content_type,
            Status status
        ) -> void {
            if (HeaderSent()) {
                return;
            }
            SetString(std::move(data), content_type);
            Base().result(status);
        }

        auto SetJsonContent(Json const& data, Status status) -> void {
            if (HeaderSent()) {
                return;
            }
            SetJson(data);
            Base().result(status);
        }

        auto SetFileContent(
            std::filesystem::path const& path,
            Headers const& request_headers
        ) -> void {
            if (HeaderSent()) {
                return;
            }
            std::error_code ec;
            auto const file_size{ std::filesystem::file_size(path, ec) };
            if (ec) {
                SetErrorContent(Status::not_found);
                return;
            }
            auto const write_time{ detail::file_last_write_time(path, ec) };
            if (ec) {
                SetErrorContent(Status::not_found);
                return;
            }

            detail::HttpRanges ranges;
            if (!ranges.Parse(request_headers[Field::range], static_cast<std::size_t>(file_size))) {
                Base().set(Field::content_range, std::format("bytes */{}", file_size));
                SetEmptyContent(Status::range_not_satisfiable);
                return;
            }

            // httplib emitted W/size-time without the quotes RFC 9110 requires.
            auto const etag{ std::format("W/\"{}-{}\"", file_size, write_time) };
            auto const last_modified{ detail::format_http_gmt_date(write_time) };
            if (request_headers[Field::if_none_match] == etag || request_headers[Field::if_modified_since] == last_modified) {
                Base().set(Field::etag, etag);
                Base().set(Field::last_modified, last_modified);
                SetEmptyContent(Status::not_modified);
                return;
            }

            std::ifstream file{ path, std::ios::binary | std::ios::in };
            if (!file.is_open()) {
                SetErrorContent(Status::forbidden);
                return;
            }

            Base().set(Field::etag, etag);
            Base().set(Field::last_modified, last_modified);
            Base().result(ranges.Empty() ? Status::ok : Status::partial_content);
            if (ranges.Empty()) {
                Base().set(Field::accept_ranges, "bytes");
            } else if (ranges.Size() == 1) {
                auto const& [first, last]{ ranges.Front() };
                Base().set(Field::content_range, std::format("bytes {}-{}/{}", first, last, file_size));
            }
            SetFile(std::move(file), std::string{ detail::get_mime_type(path.extension().string()) }, std::move(ranges));
        }

        auto SetFormDataContent(std::vector<FormData::Field> fields) -> void {
            if (HeaderSent()) {
                return;
            }
            FormData value;
            value.boundary = detail::generate_boundary();
            value.fields = std::move(fields);
            // Files referenced by a response belong to the caller.
            value.params.remove_uploaded_files = false;
            SetFormData(std::move(value));
            Base().result(Status::ok);
        }

        auto SetRedirect(std::string_view url, Status status) -> void {
            Base().set(Field::location, url);
            SetEmptyContent(status);
        }
    };

    auto StreamWriter::WriteHeader(
        Status status,
        Headers const& headers,
        Mode mode
    ) -> net::awaitable<void> {
        boost::system::error_code ec;
        co_await WriteHeader(status, headers, mode, ec);
        if (ec) {
            throw boost::system::system_error{ ec };
        }
    }

    auto StreamWriter::WriteHeader(
        Status status,
        Headers const& headers,
        Mode mode,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        // Holding the state across the co_await keeps it alive even if the Response dies meanwhile.
        auto const response{ m_response.lock() };
        if (!response) {
            ec = net::error::operation_aborted;
            co_return;
        }
        response->Base().result(status);
        response->ReplaceFields(headers);
        m_mode = mode;
        if (mode == Mode::Chunked) {
            // httplib also forced keep-alive here, overriding a client's "Connection: close".
            response->Chunked(true);
        }
        ec = co_await response->BeginStream();
    }

    auto StreamWriter::WriteBody(net::const_buffer data, bool more) -> net::awaitable<void> {
        boost::system::error_code ec;
        co_await WriteBody(data, more, ec);
        if (ec) {
            throw boost::system::system_error{ ec };
        }
    }

    auto StreamWriter::WriteBody(
        net::const_buffer data,
        bool more,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        auto const response{ m_response.lock() };
        if (!response) {
            ec = net::error::operation_aborted;
            co_return;
        }
        // Both modes send bytes as given: Content-Encoding is not applied (no compression port).
        ec = co_await response->WriteRaw(data, more);
    }

    Response::Response(std::shared_ptr<ResponseImpl> impl) : m_impl{ std::move(impl) } {}

    Response::~Response() = default;

    auto Response::Base() const -> Headers {
        return Headers::Borrow(m_impl->Base());
    }

    auto Response::operator[](Field name) const -> std::string_view {
        return detail::to_sv(m_impl->Base()[name]);
    }

    auto Response::operator[](std::string_view name) const -> std::string_view {
        return detail::to_sv(m_impl->Base()[name]);
    }

    auto Response::At(Field name) const -> std::string_view {
        return detail::to_sv(m_impl->Base().at(name));
    }

    auto Response::At(std::string_view name) const -> std::string_view {
        return detail::to_sv(m_impl->Base().at(name));
    }

    auto Response::Set(Field name, std::string_view value) -> void {
        m_impl->Base().set(name, value);
    }

    auto Response::Set(std::string_view name, std::string_view value) -> void {
        m_impl->Base().set(name, value);
    }

    auto Response::Insert(Field name, std::string_view value) -> void {
        m_impl->Base().insert(name, value);
    }

    auto Response::Insert(std::string_view name, std::string_view value) -> void {
        m_impl->Base().insert(name, value);
    }

    auto Response::Erase(Field name) -> void {
        m_impl->Base().erase(name);
    }

    auto Response::Erase(std::string_view name) -> void {
        m_impl->Base().erase(name);
    }

    auto Response::Has(Field name) const -> bool {
        return m_impl->Base().find(name) != m_impl->Base().end();
    }

    auto Response::Has(std::string_view name) const -> bool {
        return m_impl->Base().find(name) != m_impl->Base().end();
    }

    auto Response::Count(Field name) const -> std::size_t {
        return m_impl->Base().count(name);
    }

    auto Response::Count(std::string_view name) const -> std::size_t {
        return m_impl->Base().count(name);
    }

    auto Response::Result() const -> Status {
        return m_impl->Base().result();
    }

    auto Response::ResultInt() const -> unsigned {
        return m_impl->Base().result_int();
    }

    auto Response::SetEmptyContent(Status status) -> void {
        m_impl->SetEmptyContent(status);
    }

    auto Response::SetErrorContent(Status status) -> void {
        m_impl->SetErrorContent(status);
    }

    auto Response::SetStringContent(
        std::string data,
        std::string_view content_type,
        Status status
    ) -> void {
        m_impl->SetStringContent(std::move(data), content_type, status);
    }

    auto Response::SetJsonContent(Json const& data, Status status) -> void {
        m_impl->SetJsonContent(data, status);
    }

    auto Response::SetFileContent(
        std::filesystem::path const& path,
        Headers const& request_headers
    ) -> void {
        m_impl->SetFileContent(path, request_headers);
    }

    auto Response::SetFormDataContent(std::vector<FormData::Field> fields) -> void {
        m_impl->SetFormDataContent(std::move(fields));
    }

    auto Response::SetRedirect(std::string_view url, Status status) -> void {
        m_impl->SetRedirect(url, status);
    }

    auto Response::CreateSseWriter() -> std::unique_ptr<SseWriter> {
        return std::make_unique<SseWriter>(CreateStreamWriter());
    }

    auto Response::CreateNdjsonWriter() -> std::unique_ptr<NdjsonWriter> {
        return std::make_unique<NdjsonWriter>(CreateStreamWriter());
    }

    auto Response::CreateStreamWriter() -> std::shared_ptr<StreamWriter> {
        return m_impl->GetStreamWriter(m_impl);
    }

} // namespace mhttp
