/**
 * @file body_reader.cppm
 * @brief Read a message body lazily: raw streaming, or materialized into a typed BodyState.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:body_reader;

import std;
import :config;
import :error;
import :query_params;
import :form_data;
import :body_state;
import :sink;
import :connection;

namespace mhttp::detail {

    /**
     * @brief Clear a flag when leaving scope; marks a single in-flight operation.
     */
    class BusyGuard {
    private:
        bool& m_flag; ///< Flag owned by the guarded object.

    public:
        /**
         * @brief Set the flag.
         * @param flag Flag to hold.
         */
        explicit BusyGuard(bool& flag) : m_flag{ flag } {
            m_flag = true;
        }

        BusyGuard(BusyGuard const&) = delete;
        auto operator=(BusyGuard const&) -> BusyGuard& = delete;

        ~BusyGuard() {
            m_flag = false;
        }
    };

    /**
     * @brief Lazy body reader (port of httplib::detail::body_reader).
     *
     * The header parser is converted into a buffer_body parser so the body can be streamed
     * into caller buffers or fed to a Sink. Content-Encoding is not decoded (compression is not
     * ported). httplib serializes concurrent reads with an async mutex; here every reader runs
     * on the connection strand, so a second overlapping read fails with
     * Error::OperationInProgress instead of queuing.
     *
     * @tparam IsRequest True to read requests (server side), false to read responses (client).
     */
    template <bool IsRequest>
    class BodyReader {
    public:
        using ConnectionType = BasicConnection<IsRequest>;
        using HeaderParser = http::parser<IsRequest, http::empty_body>;
        using RawParser = typename ConnectionType::Parser;
        using MessageType = http::message<IsRequest, http::buffer_body>;

        static constexpr std::size_t STAGING_SIZE{ 64 * 1024 };

    private:
        std::shared_ptr<ConnectionType> m_connection; ///< Source of body bytes.
        RawParser m_parser;                           ///< Parser holding the header and body state.
        FormData::Param m_form_params;                ///< Limits for automatic multipart parsing.
        BodyState m_state;                            ///< Materialized body.
        std::function<void()> m_on_complete;          ///< Called once when the parser finishes.
        bool m_stream_started{ false };               ///< Raw bytes were handed to the caller.
        bool m_busy{ false };                         ///< A read is in flight.

    public:
        /**
         * @brief Take over a parser whose header is complete.
         * @param connection Connection that supplies body bytes.
         * @param header_parser Parser after the header read; its limits carry over.
         */
        BodyReader(
            std::shared_ptr<ConnectionType> connection,
            HeaderParser&& header_parser
        ) : m_connection{ std::move(connection) }, m_parser{ std::move(header_parser) } {}

        BodyReader(BodyReader const&) = delete;
        auto operator=(BodyReader const&) -> BodyReader& = delete;

        /**
         * @brief Borrow the message header (start line and fields).
         * @return Message owned by the parser.
         */
        [[nodiscard]] auto Message() -> MessageType& {
            return m_parser.get();
        }

        [[nodiscard]] auto Message() const -> MessageType const& {
            return m_parser.get();
        }

        /**
         * @brief Report whether the parser has consumed the whole message.
         * @return True once the last body byte was read, regardless of materialization.
         */
        [[nodiscard]] auto IsParserDone() const -> bool {
            return m_parser.is_done();
        }

        /**
         * @brief Return the declared Content-Length.
         * @return Length, or std::nullopt for chunked, close-delimited, or invalid framing.
         */
        [[nodiscard]] auto ContentLength() const -> std::optional<std::uint64_t> {
            if (auto const length{ m_parser.content_length() }) {
                return *length;
            }
            return std::nullopt;
        }

        /**
         * @brief Register a callback run once when the parser consumes the last body byte.
         * @param callback Callback; it does not run for a message that was already complete.
         */
        auto OnComplete(std::function<void()> callback) -> void {
            m_on_complete = std::move(callback);
        }

        /**
         * @brief Set the multipart limits used by automatic materialization.
         * @param params Limits and file handling.
         */
        auto SetFormDataParams(FormData::Param params) -> void {
            m_form_params = std::move(params);
        }

        /**
         * @brief Borrow the materialized body.
         * @return Body state.
         */
        [[nodiscard]] auto State() -> BodyState& {
            return m_state;
        }

        [[nodiscard]] auto State() const -> BodyState const& {
            return m_state;
        }

        /**
         * @brief Report whether the body was fully read.
         * @return False while a read is in flight, so callers keep reading instead of dropping data.
         */
        [[nodiscard]] auto IsBodyDone() const -> bool {
            if (m_busy) {
                return false;
            }
            return m_state.Has() || m_parser.is_done();
        }

        /**
         * @brief Read the next raw body bytes.
         * @param buffer Destination.
         * @param ec Receives read errors or Error::OperationInProgress.
         * @return Bytes read; 0 with no error means the body is complete.
         */
        auto ReadSomeRaw(
            net::mutable_buffer buffer,
            boost::system::error_code& ec
        ) -> net::awaitable<std::size_t> {
            if (m_busy) {
                ec = Error::OperationInProgress;
                co_return 0;
            }
            BusyGuard const guard{ m_busy };
            co_return co_await readSomeRawLocked(buffer, ec);
        }

        /**
         * @brief Read the rest of the body and materialize it by Content-Type.
         * @return Error code; a body-less request materializes as BodyType::Empty.
         */
        auto ReadBody() -> net::awaitable<boost::system::error_code> {
            co_return co_await readBody(nullptr);
        }

        /**
         * @brief Read the rest of the body as a string and move it out.
         * @return Body text, or the read error.
         */
        auto ReadString() -> net::awaitable<Result<std::string>> {
            co_return co_await readAs<std::string>(std::make_unique<StringSink>());
        }

        /**
         * @brief Read the rest of the body as JSON and move it out.
         * @return Parsed document, or the read error / Error::InvalidJson.
         */
        auto ReadJson() -> net::awaitable<Result<Json>> {
            co_return co_await readAs<Json>(std::make_unique<JsonSink>());
        }

        /**
         * @brief Read the rest of the body as form-urlencoded parameters and move them out.
         * @return Parameters, or the read error.
         */
        auto ReadQueryParams() -> net::awaitable<Result<QueryParams>> {
            co_return co_await readAs<QueryParams>(std::make_unique<QueryParamsSink>());
        }

        /**
         * @brief Read the rest of the body as multipart/form-data and move it out.
         * @return Form, or the read / multipart error.
         */
        auto ReadFormData() -> net::awaitable<Result<FormData>> {
            std::string content_type{ to_sv(m_parser.get()[http::field::content_type]) };
            co_return co_await readAs<FormData>(std::make_unique<FormDataSink>(std::move(content_type), m_form_params));
        }

        /**
         * @brief Stream the rest of the body into a file.
         * @param path Destination; replaced if it exists, removed again if the read fails.
         * @return Error code.
         */
        auto ReadToFile(std::filesystem::path path) -> net::awaitable<boost::system::error_code> {
            co_return co_await readBody(std::make_unique<FileSink>(std::move(path)));
        }

    private:
        /**
         * @brief Materialize into a specific sink and move the result out.
         * @tparam T Result type the sink commits.
         * @param sink Sink to use.
         * @return Result, Error::BodyTypeMismatch if the body was already materialized differently.
         */
        template <typename T>
        auto readAs(SinkPtr sink) -> net::awaitable<Result<T>> {
            if (auto const ec{ co_await readBody(std::move(sink)) }) {
                co_return std::unexpected{ ec };
            }
            if (!m_state.Holds<T>()) {
                co_return std::unexpected{ make_error_code(Error::BodyTypeMismatch) };
            }
            co_return m_state.Take<T>();
        }

        /**
         * @brief Read the remaining body into a sink and store the result.
         * @param sink Sink to use; nullptr chooses one from Content-Type.
         * @return Error code.
         */
        auto readBody(SinkPtr sink) -> net::awaitable<boost::system::error_code> {
            if (m_state.Has()) {
                co_return boost::system::error_code{};
            }
            if (m_stream_started) {
                co_return make_error_code(Error::BodyAlreadyStreamed);
            }
            if (m_busy) {
                co_return make_error_code(Error::OperationInProgress);
            }
            BusyGuard const guard{ m_busy };

            if (!sink && m_parser.is_done()) {
                m_state.Set<EmptyTag>();
                co_return boost::system::error_code{};
            }
            if (!sink) {
                sink = make_sink_for(to_sv(m_parser.get()[http::field::content_type]), m_form_params);
            }

            boost::system::error_code ec;
            std::optional<std::uint64_t> content_length;
            if (auto const length{ m_parser.content_length() }) {
                content_length = *length;
            }
            sink->Init(content_length, ec);
            if (ec) {
                co_return ec;
            }

            auto staging{ std::make_unique<char[]>(STAGING_SIZE) };
            for (;;) {
                auto const n{ co_await readSomeRawLocked(net::buffer(staging.get(), STAGING_SIZE), ec) };
                if (ec) {
                    co_return ec;
                }
                if (n == 0) {
                    break;
                }
                sink->Put(std::string_view{ staging.get(), n }, ec);
                if (ec) {
                    co_return ec;
                }
            }
            sink->Finish(ec);
            if (ec) {
                co_return ec;
            }
            BodyState out;
            sink->Commit(out);
            m_state = std::move(out);
            co_return boost::system::error_code{};
        }

        /**
         * @brief Read raw body bytes into `buffer`; the caller holds the busy flag.
         * @param buffer Destination.
         * @param ec Receives read errors.
         * @return Bytes read; 0 with no error means the body is complete.
         */
        auto readSomeRawLocked(
            net::mutable_buffer buffer,
            boost::system::error_code& ec
        ) -> net::awaitable<std::size_t> {
            ec = {};
            // A zero-size buffer would make the parser report need_buffer forever.
            if (buffer.size() == 0) {
                co_return 0;
            }
            for (;;) {
                if (m_parser.is_done()) {
                    ec = {};
                    co_return 0;
                }
                m_stream_started = true;
                auto& body{ m_parser.get().body() };
                body.data = buffer.data();
                body.size = buffer.size();

                co_await m_connection->ReadSome(m_parser, ec);
                if (ec == http::error::need_buffer) {
                    ec = {};
                }
                if (m_parser.is_done() && m_on_complete) {
                    std::exchange(m_on_complete, nullptr)();
                }
                auto const consumed{ buffer.size() - body.size };
                if (consumed > 0) {
                    co_return consumed;
                }
                if (ec) {
                    co_return 0;
                }
            }
        }
    };

} // namespace mhttp::detail
