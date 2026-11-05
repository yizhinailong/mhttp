/**
 * @file client_response.cppm
 * @brief Provide the client-side response with lazy body reads and SSE / NDJSON stream readers.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

export module mhttp:client_response;

import std;
import nlohmann.json;
import :config;
import :error;
import :query_params;
import :form_data;
import :headers;
import :body_state;
import :body_reader;
import :client_impl;

namespace mhttp::client {

    class ResponseImpl;

} // namespace mhttp::client

export namespace mhttp::client {

    /**
     * @brief One Server-Sent Event.
     */
    struct SseEvent {
        std::string id;                           ///< "id" field of this event.
        std::string event;                        ///< "event" field; empty means "message".
        std::string data;                         ///< "data" lines joined with '\n'.
        std::chrono::milliseconds retry{ 0 };     ///< "retry" field; 0 when absent.
    };

} // namespace mhttp::client

namespace mhttp::detail {

    /**
     * @brief Incremental text/event-stream parser (port of httplib's sse_event_parser).
     *
     * An event is dispatched at a blank line when any field was set, so a retry-only block is
     * reported too. A line without ':' is a field with an empty value (httplib ignored it), as
     * the HTML Living Standard specifies.
     */
    class SseEventParser {
    private:
        static constexpr std::size_t COMPACT_THRESHOLD{ 4096 };

        std::string m_buffer;                  ///< Unconsumed input.
        std::size_t m_start{ 0 };              ///< Start of the current line in m_buffer.
        std::deque<client::SseEvent> m_events; ///< Parsed, not yet returned events.
        client::SseEvent m_current;            ///< Event being assembled.

    public:
        /**
         * @brief Consume more stream bytes.
         * @param data Bytes in any split.
         */
        auto Feed(std::string_view data) -> void {
            m_buffer.append(data);
            for (;;) {
                auto const newline{ m_buffer.find('\n', m_start) };
                if (newline == std::string::npos) {
                    break;
                }
                auto line{ std::string_view{ m_buffer }.substr(m_start, newline - m_start) };
                if (line.ends_with('\r')) {
                    line.remove_suffix(1);
                }
                processLine(line);
                m_start = newline + 1;
            }
            if (m_start >= COMPACT_THRESHOLD) {
                m_buffer.erase(0, m_start);
                m_start = 0;
            }
        }

        [[nodiscard]] auto HasEvent() const -> bool {
            return !m_events.empty();
        }

        /**
         * @brief Take the oldest parsed event.
         * @return Event; call only when HasEvent().
         */
        auto Next() -> client::SseEvent {
            auto event{ std::move(m_events.front()) };
            m_events.pop_front();
            return event;
        }

    private:
        /**
         * @brief Apply one line without its terminator.
         * @param line Line text.
         */
        auto processLine(std::string_view line) -> void {
            if (line.empty()) {
                if (!m_current.id.empty() || !m_current.event.empty() || !m_current.data.empty() || m_current.retry.count() != 0) {
                    m_events.push_back(std::exchange(m_current, client::SseEvent{}));
                }
                return;
            }
            if (line.front() == ':') {
                return;
            }
            auto const colon{ line.find(':') };
            auto const field{ line.substr(0, colon) };
            std::string_view value;
            if (colon != std::string_view::npos) {
                value = line.substr(colon + 1);
                if (value.starts_with(' ')) {
                    value.remove_prefix(1);
                }
            }
            if (field == "id") {
                m_current.id = std::string{ value };
            } else if (field == "event") {
                m_current.event = std::string{ value };
            } else if (field == "data") {
                if (!m_current.data.empty()) {
                    m_current.data += '\n';
                }
                m_current.data += value;
            } else if (field == "retry") {
                std::uint64_t ms{ 0 };
                auto const [end, ec]{ std::from_chars(value.data(), value.data() + value.size(), ms) };
                if (!value.empty() && ec == std::errc{} && end == value.data() + value.size()) {
                    m_current.retry = std::chrono::milliseconds{ ms };
                }
            }
        }
    };

} // namespace mhttp::detail

export namespace mhttp::client {

    /**
     * @brief Read Server-Sent Events from a response body; created by Response::CreateSseReader().
     */
    class SseReader {
    private:
        std::shared_ptr<ResponseImpl> m_response; ///< Response whose body is read.
        detail::SseEventParser m_parser;          ///< Event parser.
        std::array<char, 4096> m_buffer{};        ///< Read buffer.

    public:
        /**
         * @brief Take over a response body.
         * @param response Response state.
         */
        explicit SseReader(std::shared_ptr<ResponseImpl> response) : m_response{ std::move(response) } {}

        /**
         * @brief Read the next event.
         * @return Event, std::nullopt at the end of the stream, or the read error.
         * @note httplib returned an empty event at the end, indistinguishable from data.
         */
        auto ReadEvent() -> net::awaitable<mhttp::Result<std::optional<SseEvent>>>;

        /**
         * @brief Report whether every event was returned.
         * @return True once the body ended and no parsed event is left.
         */
        [[nodiscard]] auto IsDone() const -> bool;
    };

    /**
     * @brief Read newline-delimited JSON from a response body; see Response::CreateNdjsonReader().
     */
    class NdjsonReader {
    private:
        std::shared_ptr<ResponseImpl> m_response; ///< Response whose body is read.
        std::string m_buffer;                     ///< Bytes of incomplete lines.
        std::size_t m_scan{ 0 };                  ///< Bytes of m_buffer known to hold no '\n'.
        std::array<char, 4096> m_read_buffer{};   ///< Read buffer.

    public:
        /**
         * @brief Take over a response body.
         * @param response Response state.
         */
        explicit NdjsonReader(std::shared_ptr<ResponseImpl> response) : m_response{ std::move(response) } {}

        /**
         * @brief Read the next document; blank lines are skipped, a final unterminated line counts.
         * @return Document, std::nullopt at the end of the stream, the read error, or
         *         Error::InvalidJson for a malformed line.
         * @note httplib signaled the end with a null document, which a "null" line also yields.
         */
        auto Read() -> net::awaitable<mhttp::Result<std::optional<Json>>>;

        /**
         * @brief Report whether every document was returned.
         * @return True once the body ended and no buffered bytes are left.
         */
        [[nodiscard]] auto IsDone() const -> bool;

    private:
        /**
         * @brief Parse one line.
         * @param line Line without terminator.
         * @return Document or Error::InvalidJson.
         */
        static auto parseLine(std::string_view line) -> mhttp::Result<std::optional<Json>>;
    };

    /**
     * @brief One HTTP response received by a client.
     *
     * The header is always complete. An eager send also read the body (use As*()); after a
     * lazy send, read it with Read*(), ReadSomeRaw(), ReadToFile(), or a stream reader. The client
     * stays busy until the body is read to the end; destroying the response before that closes
     * the connection. A default-constructed or moved-from response is not Valid(): queries
     * return empty values, reads return Error::NotInitialized, and As*() and the reader factories
     * throw it as boost::system::system_error.
     */
    class Response {
    private:
        std::shared_ptr<ResponseImpl> m_impl; ///< Response state; null when not valid.

    public:
        Response() = default;

        /**
         * @brief Wrap response state; created by the client.
         * @param impl Response state.
         */
        explicit Response(std::shared_ptr<ResponseImpl> impl) : m_impl{ std::move(impl) } {}

        Response(Response&& other) noexcept = default;
        auto operator=(Response&& other) noexcept -> Response& = default;
        ~Response() = default;

        [[nodiscard]] auto Valid() const noexcept -> bool {
            return m_impl != nullptr;
        }

        /**
         * @brief Return the status.
         * @return Status, or Status::unknown when not valid.
         */
        [[nodiscard]] auto Result() const -> Status;
        [[nodiscard]] auto ResultInt() const -> unsigned;

        /**
         * @brief Return the HTTP version.
         * @return 11 for HTTP/1.1, 10 for HTTP/1.0, 0 when not valid.
         */
        [[nodiscard]] auto Version() const -> unsigned;

        [[nodiscard]] auto operator[](Field name) const -> std::string_view;
        [[nodiscard]] auto operator[](std::string_view name) const -> std::string_view;

        /**
         * @brief Return a header value.
         * @param name Field to look up.
         * @return Value.
         * @throws std::out_of_range If the field is absent or the response is not valid.
         */
        [[nodiscard]] auto At(Field name) const -> std::string_view;
        [[nodiscard]] auto At(std::string_view name) const -> std::string_view;

        [[nodiscard]] auto Has(Field name) const -> bool;
        [[nodiscard]] auto Has(std::string_view name) const -> bool;
        [[nodiscard]] auto Count(Field name) const -> std::size_t;
        [[nodiscard]] auto Count(std::string_view name) const -> std::size_t;

        /**
         * @brief Borrow every response header.
         * @return View into this response, or an empty owned collection when not valid.
         */
        [[nodiscard]] auto Base() const -> Headers;

        /**
         * @brief Return the declared Content-Length.
         * @return Length, or std::nullopt when absent or not valid.
         */
        [[nodiscard]] auto ContentLength() const -> std::optional<std::uint64_t>;

        /**
         * @brief Borrow the materialized body as text.
         * @return Body; empty for body-less responses.
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

        auto ReadString() -> net::awaitable<mhttp::Result<std::string>>;
        auto ReadJson() -> net::awaitable<mhttp::Result<Json>>;
        auto ReadFormData() -> net::awaitable<mhttp::Result<FormData>>;
        auto ReadQueryParams() -> net::awaitable<mhttp::Result<QueryParams>>;

        /**
         * @brief Read the rest of the body and materialize it by Content-Type (see As*()).
         * @return Error code.
         */
        auto ReadBody() -> net::awaitable<boost::system::error_code>;

        /**
         * @brief Read and discard the rest of the body in small chunks.
         * @return Error code; on success the connection can serve the next request.
         */
        auto Drain() -> net::awaitable<boost::system::error_code>;

        /**
         * @brief Stream the rest of the body into a file.
         * @param path Destination; replaced if it exists, removed again if the read fails.
         * @return Error code.
         */
        auto ReadToFile(
            std::filesystem::path const& path
        ) -> net::awaitable<boost::system::error_code>;

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
         * @return True once every body byte was consumed or materialized; true when not valid.
         */
        [[nodiscard]] auto IsBodyDone() const -> bool;

        /**
         * @brief Read the body as Server-Sent Events.
         * @return Reader sharing this response's state.
         * @throws boost::system::system_error Error::NotInitialized when not valid.
         */
        [[nodiscard]] auto CreateSseReader() -> std::unique_ptr<SseReader>;

        /**
         * @brief Read the body as newline-delimited JSON.
         * @return Reader sharing this response's state.
         * @throws boost::system::system_error Error::NotInitialized when not valid.
         */
        [[nodiscard]] auto CreateNdjsonReader() -> std::unique_ptr<NdjsonReader>;

        friend auto get_impl(Response& response) -> ResponseImpl& {
            return *response.m_impl;
        }

    private:
        /**
         * @brief Return the state or throw for an invalid response.
         * @return Response state.
         * @throws boost::system::system_error Error::NotInitialized when not valid.
         */
        [[nodiscard]] auto checked() const -> ResponseImpl&;
    };

} // namespace mhttp::client

namespace mhttp::client {

    /**
     * @brief Response state behind client::Response: the body reader and the owning client.
     */
    class ResponseImpl : public detail::BodyReader<false> {
    private:
        std::shared_ptr<detail::ClientImpl> m_client; ///< Connection the body is read from.

    public:
        /**
         * @brief Take over a parsed response header.
         * @param client Client connection.
         * @param header_parser Parser holding the header.
         */
        ResponseImpl(
            std::shared_ptr<detail::ClientImpl> client,
            HeaderParser&& header_parser
        ) : BodyReader{ client, std::move(header_parser) }, m_client{ std::move(client) } {}

        /**
         * @brief Close the connection when the body was not read to the end.
         */
        ~ResponseImpl() {
            if (!IsParserDone()) {
                m_client->AbandonBody();
            }
        }

        /**
         * @brief Build a response and hook the end of its body into the client's bookkeeping.
         * @param client Client connection.
         * @param header_parser Parser holding the header.
         * @param close_when_done Close after the body, e.g. when the request was not finished.
         * @return Response.
         */
        static auto Create(
            std::shared_ptr<detail::ClientImpl> const& client,
            std::unique_ptr<HeaderParser> header_parser,
            bool close_when_done = false
        ) -> Response {
            auto impl{ std::make_shared<ResponseImpl>(client, std::move(*header_parser)) };
            if (impl->IsParserDone()) {
                if (close_when_done) {
                    client->AbandonBody();
                }
            } else {
                impl->OnComplete([client, close_when_done]() {
                    if (close_when_done) {
                        client->AbandonBody();
                    } else {
                        client->EndBody();
                    }
                });
            }
            return Response{ std::move(impl) };
        }
    };

    // ---- SseReader ----

    auto SseReader::ReadEvent() -> net::awaitable<mhttp::Result<std::optional<SseEvent>>> {
        while (!m_parser.HasEvent() && !m_response->IsBodyDone()) {
            boost::system::error_code ec;
            auto const bytes{ co_await m_response->ReadSomeRaw(net::buffer(m_buffer), ec) };
            if (ec) {
                co_return std::unexpected{ ec };
            }
            if (bytes == 0) {
                break;
            }
            m_parser.Feed(std::string_view{ m_buffer.data(), bytes });
        }
        if (m_parser.HasEvent()) {
            co_return m_parser.Next();
        }
        co_return std::nullopt;
    }

    auto SseReader::IsDone() const -> bool {
        return m_response->IsBodyDone() && !m_parser.HasEvent();
    }

    // ---- NdjsonReader ----

    auto NdjsonReader::Read() -> net::awaitable<mhttp::Result<std::optional<Json>>> {
        for (;;) {
            if (auto const newline{ m_buffer.find('\n', m_scan) }; newline != std::string::npos) {
                auto line{ std::string_view{ m_buffer }.substr(0, newline) };
                if (line.ends_with('\r')) {
                    line.remove_suffix(1);
                }
                bool const blank{ line.empty() };
                auto result{ blank ? mhttp::Result<std::optional<Json>>{ std::nullopt } : parseLine(line) };
                m_buffer.erase(0, newline + 1);
                m_scan = 0;
                if (blank) {
                    continue;
                }
                co_return result;
            }
            m_scan = m_buffer.size();
            if (m_response->IsBodyDone()) {
                // A final line without '\n' is a record (RFC 7464 allows it); httplib dropped it.
                auto tail{ std::exchange(m_buffer, std::string{}) };
                m_scan = 0;
                if (tail.ends_with('\r')) {
                    tail.pop_back();
                }
                if (tail.empty()) {
                    co_return std::nullopt;
                }
                co_return parseLine(tail);
            }
            boost::system::error_code ec;
            auto const bytes{ co_await m_response->ReadSomeRaw(net::buffer(m_read_buffer), ec) };
            if (ec) {
                co_return std::unexpected{ ec };
            }
            m_buffer.append(m_read_buffer.data(), bytes);
        }
    }

    auto NdjsonReader::IsDone() const -> bool {
        return m_response->IsBodyDone() && m_buffer.empty();
    }

    auto NdjsonReader::parseLine(std::string_view line) -> mhttp::Result<std::optional<Json>> {
        auto value = Json::parse(line.begin(), line.end(), nullptr, false);
        if (value.is_discarded()) {
            return std::unexpected{ make_error_code(Error::InvalidJson) };
        }
        return std::optional<Json>(std::move(value));
    }

    // ---- Response ----

    auto Response::Result() const -> Status {
        return m_impl ? m_impl->Message().result() : Status::unknown;
    }

    auto Response::ResultInt() const -> unsigned {
        return m_impl ? m_impl->Message().result_int() : 0U;
    }

    auto Response::Version() const -> unsigned {
        return m_impl ? m_impl->Message().version() : 0U;
    }

    auto Response::operator[](Field name) const -> std::string_view {
        return m_impl ? detail::to_sv(m_impl->Message()[name]) : std::string_view{};
    }

    auto Response::operator[](std::string_view name) const -> std::string_view {
        return m_impl ? detail::to_sv(m_impl->Message()[name]) : std::string_view{};
    }

    auto Response::At(Field name) const -> std::string_view {
        if (!m_impl) {
            throw std::out_of_range{ "client::Response::At: response is not valid" };
        }
        return detail::to_sv(m_impl->Message().at(name));
    }

    auto Response::At(std::string_view name) const -> std::string_view {
        if (!m_impl) {
            throw std::out_of_range{ "client::Response::At: response is not valid" };
        }
        return detail::to_sv(m_impl->Message().at(name));
    }

    auto Response::Has(Field name) const -> bool {
        return m_impl && m_impl->Message().find(name) != m_impl->Message().end();
    }

    auto Response::Has(std::string_view name) const -> bool {
        return m_impl && m_impl->Message().find(name) != m_impl->Message().end();
    }

    auto Response::Count(Field name) const -> std::size_t {
        return m_impl ? m_impl->Message().count(name) : 0;
    }

    auto Response::Count(std::string_view name) const -> std::size_t {
        return m_impl ? m_impl->Message().count(name) : 0;
    }

    auto Response::Base() const -> Headers {
        return m_impl ? Headers::Borrow(m_impl->Message().base()) : Headers{};
    }

    auto Response::ContentLength() const -> std::optional<std::uint64_t> {
        return m_impl ? m_impl->ContentLength() : std::nullopt;
    }

    auto Response::AsString() const -> std::string const& {
        return checked().State().As<std::string>();
    }

    auto Response::AsJson() const -> Json const& {
        return checked().State().As<Json>();
    }

    auto Response::AsFormData() const -> FormData const& {
        return checked().State().As<FormData>();
    }

    auto Response::AsQueryParams() const -> QueryParams const& {
        return checked().State().As<QueryParams>();
    }

    auto Response::Type() const -> BodyType {
        return m_impl ? m_impl->State().Type() : BodyType::None;
    }

    auto Response::ReadString() -> net::awaitable<mhttp::Result<std::string>> {
        if (!m_impl) {
            co_return std::unexpected{ make_error_code(Error::NotInitialized) };
        }
        co_return co_await m_impl->ReadString();
    }

    auto Response::ReadJson() -> net::awaitable<mhttp::Result<Json>> {
        if (!m_impl) {
            co_return std::unexpected{ make_error_code(Error::NotInitialized) };
        }
        co_return co_await m_impl->ReadJson();
    }

    auto Response::ReadFormData() -> net::awaitable<mhttp::Result<FormData>> {
        if (!m_impl) {
            co_return std::unexpected{ make_error_code(Error::NotInitialized) };
        }
        co_return co_await m_impl->ReadFormData();
    }

    auto Response::ReadQueryParams() -> net::awaitable<mhttp::Result<QueryParams>> {
        if (!m_impl) {
            co_return std::unexpected{ make_error_code(Error::NotInitialized) };
        }
        co_return co_await m_impl->ReadQueryParams();
    }

    auto Response::ReadBody() -> net::awaitable<boost::system::error_code> {
        if (!m_impl) {
            co_return make_error_code(Error::NotInitialized);
        }
        co_return co_await m_impl->ReadBody();
    }

    auto Response::Drain() -> net::awaitable<boost::system::error_code> {
        if (!m_impl) {
            co_return make_error_code(Error::NotInitialized);
        }
        // Fixed-size discard buffer: a large redirect body never lands in memory as a whole.
        std::array<char, 8192> buffer;
        for (;;) {
            boost::system::error_code ec;
            auto const bytes{ co_await m_impl->ReadSomeRaw(net::buffer(buffer), ec) };
            if (ec) {
                co_return ec;
            }
            if (bytes == 0) {
                co_return boost::system::error_code{};
            }
        }
    }

    auto Response::ReadToFile(
        std::filesystem::path const& path
    ) -> net::awaitable<boost::system::error_code> {
        if (!m_impl) {
            co_return make_error_code(Error::NotInitialized);
        }
        co_return co_await m_impl->ReadToFile(path);
    }

    auto Response::ReadSomeRaw(
        net::mutable_buffer buffer,
        boost::system::error_code& ec
    ) -> net::awaitable<std::size_t> {
        if (!m_impl) {
            ec = make_error_code(Error::NotInitialized);
            co_return 0;
        }
        co_return co_await m_impl->ReadSomeRaw(buffer, ec);
    }

    auto Response::ReadSomeRaw(net::mutable_buffer buffer) -> net::awaitable<std::size_t> {
        boost::system::error_code ec;
        auto const bytes{ co_await ReadSomeRaw(buffer, ec) };
        if (ec) {
            throw boost::system::system_error{ ec };
        }
        co_return bytes;
    }

    auto Response::IsBodyDone() const -> bool {
        return !m_impl || m_impl->IsBodyDone();
    }

    auto Response::CreateSseReader() -> std::unique_ptr<SseReader> {
        static_cast<void>(checked());
        return std::make_unique<SseReader>(m_impl);
    }

    auto Response::CreateNdjsonReader() -> std::unique_ptr<NdjsonReader> {
        static_cast<void>(checked());
        return std::make_unique<NdjsonReader>(m_impl);
    }

    auto Response::checked() const -> ResponseImpl& {
        if (!m_impl) {
            throw boost::system::system_error{ make_error_code(Error::NotInitialized) };
        }
        return *m_impl;
    }

} // namespace mhttp::client
