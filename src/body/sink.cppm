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
