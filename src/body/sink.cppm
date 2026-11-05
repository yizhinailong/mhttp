/**
 * @file sink.cppm
 * @brief Consume body bytes incrementally and materialize them into a BodyState.
 */
module;

#include <boost/beast/http/error.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:sink;

import std;
import nlohmann.json;
import :config;
import :error;
import :query_params;
import :form_data;
import :body_state;
import :multipart_parser;

namespace mhttp::detail {

    /**
     * @brief Incremental consumer of body bytes, chosen by Content-Type.
     */
    class Sink {
    public:
        virtual ~Sink() = default;

        /**
         * @brief Prepare for a body.
         * @param content_length Declared length; empty for chunked bodies.
         * @param ec Receives setup errors.
         */
        virtual auto Init(
            std::optional<std::uint64_t> content_length,
            boost::system::error_code& ec
        ) -> void {
            ec = {};
            static_cast<void>(content_length);
        }

        /**
         * @brief Consume one chunk.
         * @param chunk Body bytes.
         * @param ec Receives parse errors.
         */
        virtual auto Put(std::string_view chunk, boost::system::error_code& ec) -> void = 0;

        /**
         * @brief Finish after the last chunk.
         * @param ec Receives parse errors.
         */
        virtual auto Finish(boost::system::error_code& ec) -> void = 0;

        /**
         * @brief Move the result into a body state.
         * @param out Destination.
         */
        virtual auto Commit(BodyState& out) -> void = 0;
    };

    using SinkPtr = std::unique_ptr<Sink>;

    /**
     * @brief Accumulate bytes into a std::string.
     */
    class StringSink : public Sink {
    private:
        std::string m_buffer; ///< Accumulated body.

    public:
        auto Init(
            std::optional<std::uint64_t> content_length,
            boost::system::error_code& ec
        ) -> void override {
            ec = {};
            if (content_length) {
                m_buffer.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(*content_length, 1024 * 1024)));
            }
        }

        auto Put(std::string_view chunk, boost::system::error_code& ec) -> void override {
            ec = {};
            m_buffer.append(chunk);
        }

        auto Finish(boost::system::error_code& ec) -> void override {
            ec = {};
        }

        auto Commit(BodyState& out) -> void override {
            out.Set<std::string>(std::move(m_buffer));
        }
    };

    /**
     * @brief Accumulate bytes and parse them as JSON at the end.
     * @note nlohmann::json has no push parser, so the text is buffered before parsing.
     */
    class JsonSink : public Sink {
    private:
        std::string m_buffer; ///< Accumulated JSON text.
        Json m_value;         ///< Parsed document.

    public:
        auto Put(std::string_view chunk, boost::system::error_code& ec) -> void override {
            ec = {};
            m_buffer.append(chunk);
        }

        auto Finish(boost::system::error_code& ec) -> void override {
            ec = {};
            m_value = Json::parse(m_buffer, nullptr, false);
            if (m_value.is_discarded()) {
                ec = Error::InvalidJson;
            }
        }

        auto Commit(BodyState& out) -> void override {
            out.Set<Json>(std::move(m_value));
        }
    };

    /**
     * @brief Accumulate bytes and decode them as application/x-www-form-urlencoded.
     */
    class QueryParamsSink : public Sink {
    private:
        std::string m_buffer;  ///< Accumulated encoded text.
        QueryParams m_params;  ///< Decoded parameters.

    public:
        auto Put(std::string_view chunk, boost::system::error_code& ec) -> void override {
            ec = {};
            m_buffer.append(chunk);
        }

        auto Finish(boost::system::error_code& ec) -> void override {
            ec = {};
            if (!m_params.Decode(m_buffer)) {
                ec = http::error::unexpected_body;
            }
        }

        auto Commit(BodyState& out) -> void override {
            out.Set<QueryParams>(std::move(m_params));
        }
    };

    /**
     * @brief Parse multipart/form-data incrementally.
     */
    class FormDataSink : public Sink {
    private:
        MultipartParser m_parser; ///< Multipart state machine.

    public:
        /**
         * @brief Create a sink for one body.
         * @param content_type Content-Type with the boundary parameter.
         * @param params Limits and file handling.
         */
        FormDataSink(std::string content_type, FormData::Param params) : m_parser{ std::move(content_type), std::move(params) } {}

        auto Init(std::optional<std::uint64_t>, boost::system::error_code& ec) -> void override {
            m_parser.Reset(ec);
        }

        auto Put(std::string_view chunk, boost::system::error_code& ec) -> void override {
            m_parser.Put(chunk, ec);
        }

        auto Finish(boost::system::error_code& ec) -> void override {
            m_parser.Finish(ec);
        }

        auto Commit(BodyState& out) -> void override {
            out.Set<FormData>(m_parser.Take());
        }
    };

    /**
     * @brief Write the body to a file; commits nothing to the body state.
     *
     * A body that fails part-way leaves no file behind (httplib kept the partial file).
     */
    class FileSink : public Sink {
    private:
        std::filesystem::path m_path; ///< Destination file.
        std::ofstream m_file;         ///< Open destination.
        bool m_opened{ false };       ///< Init() created the file.
        bool m_finished{ false };     ///< The whole body was written.

    public:
        /**
         * @brief Create a sink for one body.
         * @param path Destination file; replaced if it exists.
         */
        explicit FileSink(std::filesystem::path path) : m_path{ std::move(path) } {}

        FileSink(FileSink const&) = delete;
        auto operator=(FileSink const&) -> FileSink& = delete;

        ~FileSink() override {
            if (m_opened && !m_finished) {
                m_file.close();
                std::error_code ec;
                std::filesystem::remove(m_path, ec);
            }
        }

        auto Init(std::optional<std::uint64_t>, boost::system::error_code& ec) -> void override {
            ec = {};
            m_file.open(m_path, std::ios::out | std::ios::binary | std::ios::trunc);
            m_opened = m_file.is_open();
            if (!m_opened) {
                ec = boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            }
        }

        auto Put(std::string_view chunk, boost::system::error_code& ec) -> void override {
            ec = {};
            m_file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (!m_file) {
                ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
        }

        auto Finish(boost::system::error_code& ec) -> void override {
            ec = {};
            m_file.close();
            if (m_file.fail()) {
                ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
                return;
            }
            m_finished = true;
        }

        auto Commit(BodyState&) -> void override {}
    };

    /**
     * @brief Choose the default sink for a Content-Type.
     * @param content_type Request Content-Type.
     * @param params Multipart limits and file handling.
     * @return Sink for multipart, JSON, form-urlencoded, or (otherwise) string bodies.
     */
    auto make_sink_for(std::string_view content_type, FormData::Param params) -> SinkPtr {
        if (content_type.starts_with("multipart/form-data")) {
            return std::make_unique<FormDataSink>(std::string{ content_type }, std::move(params));
        }
        if (content_type.starts_with("application/json")) {
            return std::make_unique<JsonSink>();
        }
        if (content_type.starts_with("application/x-www-form-urlencoded")) {
            return std::make_unique<QueryParamsSink>();
        }
        return std::make_unique<StringSink>();
    }

} // namespace mhttp::detail
