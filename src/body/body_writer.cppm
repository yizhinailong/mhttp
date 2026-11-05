/**
 * @file body_writer.cppm
 * @brief Frame and write a message body: whole-message writes and header-then-chunks streaming.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:body_writer;

import std;
import :config;
import :error;
import :http_date;
import :query_params;
import :form_data;
import :headers;
import :http_ranges;
import :source;
import :connection;
import :body_reader;

namespace mhttp::detail {

    /**
     * @brief Body writer (port of httplib::detail::body_writer).
     *
     * Content setters attach a Source; PreparePayload() derives framing from it; WriteMessage()
     * sends header plus body, while BeginStream()/WriteRaw() stream chunks supplied by the
     * caller. Content-Encoding is not applied (compression is not ported). Overlapping writes
     * fail with Error::OperationInProgress instead of queuing on an async mutex as in httplib.
     *
     * @tparam IsRequest True to write requests (client side), false to write responses (server).
     */
    template <bool IsRequest>
    class BodyWriter {
    public:
        using ConnectionType = BasicConnection<!IsRequest>;
        using MessageType = http::message<IsRequest, http::buffer_body>;
        using HeaderType = typename MessageType::header_type;
        using Serializer = typename ConnectionType::Serializer;

    private:
        MessageType m_msg;                            ///< Header and body cursor.
        std::shared_ptr<ConnectionType> m_connection; ///< Destination; null until attached.
        SourcePtr m_source;                           ///< Body producer.
        std::unique_ptr<Serializer> m_serializer;     ///< Lazily created; references m_msg.
        bool m_header_sent{ false };                  ///< The header reached the wire (sticky per send).
        bool m_busy{ false };                         ///< A write is in flight.

    public:
        /**
         * @brief Create a writer, optionally bound to a connection.
         * @param connection Destination; may be null and attached later.
         */
        explicit BodyWriter(std::shared_ptr<ConnectionType> connection = nullptr)
            : m_connection{ std::move(connection) } {}

        BodyWriter(BodyWriter const&) = delete;
        auto operator=(BodyWriter const&) -> BodyWriter& = delete;

        /**
         * @brief Bind to the connection the next write goes to.
         * @param connection Destination.
         */
        auto Attach(std::shared_ptr<ConnectionType> connection) -> void {
            m_connection = std::move(connection);
        }

        /**
         * @brief Borrow the start line and fields.
         * @return Message header.
         */
        [[nodiscard]] auto Base() -> HeaderType& {
            return m_msg.base();
        }

        [[nodiscard]] auto Base() const -> HeaderType const& {
            return m_msg.base();
        }

        [[nodiscard]] auto KeepAlive() const -> bool {
            return m_msg.keep_alive();
        }

        auto KeepAlive(bool value) -> void {
            m_msg.keep_alive(value);
        }

        auto ContentLength(std::uint64_t value) -> void {
            m_msg.content_length(value);
        }

        auto Chunked(bool value) -> void {
            m_msg.chunked(value);
        }

        /**
         * @brief Replace header fields with the ones in `headers`, name by name.
         * @param headers Fields to apply.
         */
        auto ReplaceFields(Headers const& headers) -> void {
            for (auto const& header : headers) {
                m_msg.erase(header.name_string);
            }
            for (auto const& header : headers) {
                m_msg.insert(header.name_string, header.value);
            }
        }

        auto SetString(std::string data, std::string_view content_type) -> void {
            Reset();
            m_msg.set(http::field::content_type, content_type);
            m_source = std::make_shared<StringSource>(std::move(data));
        }

        /**
         * @brief Attach a JSON body.
         * @param data Document to serialize now, so Content-Length is known.
         * @throws nlohmann::json::type_error If a string holds invalid UTF-8.
         */
        auto SetJson(Json const& data) -> void {
            Reset();
            m_msg.set(http::field::content_type, "application/json; charset=utf-8");
            if constexpr (!IsRequest) {
                m_msg.set(http::field::cache_control, "no-store");
            }
            m_source = std::make_shared<StringSource>(data.dump());
        }

        auto SetQueryParams(QueryParams const& data) -> void {
            Reset();
            m_msg.set(http::field::content_type, "application/x-www-form-urlencoded");
            m_source = std::make_shared<StringSource>(data.Encoded());
        }

        auto SetFormData(FormData data) -> void {
            Reset();
            m_msg.set(http::field::content_type, "multipart/form-data; boundary=" + data.boundary);
            m_source = std::make_shared<FormDataSource>(std::move(data));
        }

        /**
         * @brief Attach a file body, optionally limited to byte ranges.
         * @param file Open binary stream.
         * @param content_type MIME type of the file.
         * @param ranges Validated ranges; two or more produce multipart/byteranges.
         */
        auto SetFile(std::ifstream file, std::string content_type, HttpRanges ranges) -> void {
            Reset();
            std::string boundary;
            if (ranges.Size() > 1) {
                boundary = generate_boundary();
                m_msg.set(http::field::content_type, "multipart/byteranges; boundary=" + boundary);
            } else {
                m_msg.set(http::field::content_type, content_type);
            }
            m_source = std::make_shared<FileSource>(std::move(file), std::move(ranges), std::move(content_type), std::move(boundary));
        }

        auto SetEmpty() -> void {
            Reset();
            m_source = shared_empty_source();
        }

        /**
         * @brief Drop the body and serializer state but keep the header, Content-Length included.
         */
        auto DiscardBody() -> void {
            m_serializer.reset();
            m_source.reset();
            m_msg.body() = http::buffer_body::value_type{};
        }

        /**
         * @brief Prepare for a new body: drop it together with the old Content-Length.
         */
        auto Reset() -> void {
            DiscardBody();
            m_msg.erase(http::field::content_length);
        }

        /**
         * @brief Make a fully or partly sent message ready to be sent again.
         *
         * Used by the client before every attempt: a retry on a fresh connection, or a redirect
         * that keeps the body (307/308).
         *
         * @return False when the body source cannot be replayed.
         */
        auto PrepareForSend() -> bool {
            m_serializer.reset();
            m_header_sent = false;
            m_msg.body() = http::buffer_body::value_type{};
            return !m_source || m_source->Rewind();
        }

        /**
         * @brief Derive framing from the source.
         *
         * A known source length becomes Content-Length; otherwise an explicit Content-Length is
         * kept, or Beast chooses chunked (HTTP/1.1) or close-delimited (HTTP/1.0). 1xx, 204, and
         * 304 responses get neither Content-Length nor a body (RFC 9110 §8.6; httplib sent
         * Content-Length: 0 here). Empty requests only announce Content-Length: 0 for methods
         * that define a body (RFC 9110 §8.6), as Beast's prepare_payload() does.
         */
        auto PreparePayload() -> void {
            if (statusForbidsBody()) {
                m_msg.erase(http::field::content_length);
                m_msg.chunked(false);
                return;
            }
            if (m_source) {
                if (auto const length{ m_source->ContentLength() }) {
                    m_msg.chunked(false);
                    if (*length == 0 && !emptyBodyNeedsLength()) {
                        m_msg.erase(http::field::content_length);
                    } else {
                        m_msg.content_length(*length);
                    }
                    return;
                }
                if (m_msg.has_content_length()) {
                    m_msg.chunked(false);
                    return;
                }
                m_msg.prepare_payload();
                return;
            }
            if (!m_msg.has_content_length()) {
                m_msg.chunked(false);
                if (emptyBodyNeedsLength()) {
                    m_msg.content_length(0);
                }
            }
        }

        /**
         * @brief Report whether the header reached the wire in the current send.
         * @return True after a successful header write, until PrepareForSend().
         */
        [[nodiscard]] auto HeaderSent() const noexcept -> bool {
            return m_header_sent;
        }

        /**
         * @brief Report whether the whole message (header and body) was serialized.
         * @return True when the serializer finished.
         */
        [[nodiscard]] auto MessageDone() const -> bool {
            return m_serializer && m_serializer->is_done();
        }

        /**
         * @brief Write header and body.
         * @param headers_only True for HEAD responses: the GET-identical header, no body byte.
         * @return Error code; failures clear keep-alive on responses.
         */
        auto WriteMessage(bool headers_only = false) -> net::awaitable<boost::system::error_code> {
            if (m_busy) {
                co_return make_error_code(Error::OperationInProgress);
            }
            BusyGuard const guard{ m_busy };
            if (m_serializer && m_serializer->is_header_done()) {
                co_return invalidArgument();
            }

            PreparePayload();
            boost::system::error_code ec;
            co_await m_connection->WriteHeader(serializer(), ec);
            if (ec) {
                markBroken();
                co_return ec;
            }
            m_header_sent = true;
            // HEAD stops after the header; writing the empty-body step would emit a chunked
            // terminator, and those bytes would be a body.
            if (headers_only || statusForbidsBody()) {
                co_return ec;
            }
            if (!m_source || m_source->ContentLength() == 0) {
                co_return co_await writeRawLocked({}, false);
            }
            for (;;) {
                auto const chunk{ m_source->Next(ec) };
                if (ec) {
                    markBroken();
                    co_return ec;
                }
                if (!chunk) {
                    co_return co_await writeRawLocked({}, false);
                }
                ec = co_await writeRawLocked(net::buffer(chunk->first.data(), chunk->first.size()), chunk->second);
                if (ec || !chunk->second) {
                    co_return ec;
                }
            }
        }

        /**
         * @brief Write the header and enter streaming mode; framing comes from Base().
         * @return Error code.
         */
        auto BeginStream() -> net::awaitable<boost::system::error_code> {
            if (m_busy) {
                co_return make_error_code(Error::OperationInProgress);
            }
            BusyGuard const guard{ m_busy };
            if (m_serializer && m_serializer->is_header_done()) {
                co_return invalidArgument();
            }
            DiscardBody();
            boost::system::error_code ec;
            co_await m_connection->WriteHeader(serializer(), ec);
            if (ec) {
                markBroken();
                co_return ec;
            }
            m_header_sent = true;
            co_return ec;
        }

        /**
         * @brief Write one body chunk in streaming mode.
         * @param data Body bytes.
         * @param more False for the last chunk (writes the chunked terminator).
         * @return Error code; invalid_argument outside streaming mode.
         */
        auto WriteRaw(
            net::const_buffer data,
            bool more
        ) -> net::awaitable<boost::system::error_code> {
            if (m_busy) {
                co_return make_error_code(Error::OperationInProgress);
            }
            BusyGuard const guard{ m_busy };
            if (!m_serializer || !m_serializer->is_header_done() || m_serializer->is_done()) {
                co_return invalidArgument();
            }
            co_return co_await writeRawLocked(data, more);
        }

    private:
        /**
         * @brief Return the serializer, creating it on first use.
         * @return Serializer of m_msg.
         */
        auto serializer() -> Serializer& {
            if (!m_serializer) {
                m_serializer = std::make_unique<Serializer>(m_msg);
            }
            return *m_serializer;
        }

        /**
         * @brief Report whether the status code forbids a body.
         * @return True for 1xx, 204, and 304 responses; always false for requests.
         */
        [[nodiscard]] auto statusForbidsBody() const -> bool {
            if constexpr (IsRequest) {
                return false;
            } else {
                auto const code{ m_msg.result_int() };
                return code / 100 == 1 || code == 204 || code == 304;
            }
        }

        /**
         * @brief Report whether an empty body is announced with Content-Length: 0.
         * @return Always for responses; for requests only when the method defines a body.
         */
        [[nodiscard]] auto emptyBodyNeedsLength() const -> bool {
            if constexpr (IsRequest) {
                auto const method{ m_msg.method() };
                return method == http::verb::post || method == http::verb::put || method == http::verb::patch;
            } else {
                return true;
            }
        }

        /**
         * @brief Record a failed write; a response then closes its connection.
         * @note Requests keep their header unchanged so a retry sends the same message.
         */
        auto markBroken() -> void {
            if constexpr (!IsRequest) {
                m_msg.keep_alive(false);
            }
        }

        /**
         * @brief Hand `data` to the serializer and drive it until consumed.
         * @param data Body bytes; empty with more=false finishes the message.
         * @param more Whether more body follows.
         * @return Error code.
         */
        auto writeRawLocked(
            net::const_buffer data,
            bool more
        ) -> net::awaitable<boost::system::error_code> {
            m_msg.body().data = data.size() > 0 ? const_cast<void*>(data.data()) : nullptr;
            m_msg.body().size = data.size();
            m_msg.body().more = more;

            boost::system::error_code ec;
            co_await m_connection->Write(serializer(), ec);
            if (ec == http::error::need_buffer) {
                ec = {};
            } else if (ec) {
                markBroken();
            }
            co_return ec;
        }

        static auto invalidArgument() -> boost::system::error_code {
            return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
        }
    };

} // namespace mhttp::detail
