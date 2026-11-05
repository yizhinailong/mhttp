/**
 * @file source.cppm
 * @brief Produce outgoing body bytes chunk by chunk from strings, forms, and files.
 */
module;

#include <boost/beast/http/error.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:source;

import std;
import :config;
import :form_data;
import :http_ranges;

namespace mhttp::detail {

    /**
     * @brief Incremental producer of body bytes.
     */
    class Source {
    public:
        /**
         * @brief One produced chunk and whether more chunks follow.
         */
        using Chunk = std::optional<std::pair<std::string_view, bool>>;

        virtual ~Source() = default;

        /**
         * @brief Produce the next chunk.
         * @param ec Receives I/O errors.
         * @return Next chunk valid until the following call, or std::nullopt when exhausted.
         */
        virtual auto Next(boost::system::error_code& ec) -> Chunk = 0;

        /**
         * @brief Restart production from the first byte, so a request can be sent again.
         * @return False when the source cannot be replayed.
         * @note httplib had no rewind: a request re-sent after a 307/308 redirect announced its
         *       old Content-Length but carried no body.
         */
        virtual auto Rewind() -> bool = 0;

        /**
         * @brief Report the total size when known in advance.
         * @return Body size, or std::nullopt for multipart/byteranges and other unknown sizes.
         */
        [[nodiscard]] virtual auto ContentLength() const -> std::optional<std::uint64_t> {
            return std::nullopt;
        }
    };

    using SourcePtr = std::shared_ptr<Source>;

    /**
     * @brief Produce one string in a single chunk.
     */
    class StringSource : public Source {
    private:
        std::string m_data;   ///< Body bytes.
        bool m_done{ false }; ///< The chunk was produced.

    public:
        /**
         * @brief Take ownership of the body bytes.
         * @param data Body bytes.
         */
        explicit StringSource(std::string data) : m_data{ std::move(data) } {}

        auto Next(boost::system::error_code& ec) -> Chunk override {
            ec = {};
            if (m_done) {
                return std::nullopt;
            }
            m_done = true;
            return std::pair{ std::string_view{ m_data }, false };
        }

        auto Rewind() -> bool override {
            m_done = false;
            return true;
        }

        [[nodiscard]] auto ContentLength() const -> std::optional<std::uint64_t> override {
            return m_data.size();
        }
    };

    /**
     * @brief Produce nothing; a body of length 0.
     */
    class EmptySource : public Source {
    public:
        auto Next(boost::system::error_code& ec) -> Chunk override {
            ec = {};
            return std::nullopt;
        }

        auto Rewind() -> bool override {
            return true;
        }

        [[nodiscard]] auto ContentLength() const -> std::optional<std::uint64_t> override {
            return 0;
        }
    };

    /**
     * @brief Return the shared stateless empty source.
     * @return Empty source singleton.
     */
    auto shared_empty_source() -> SourcePtr {
        static SourcePtr const s_instance{ std::make_shared<EmptySource>() };
        return s_instance;
    }

    /**
     * @brief Serialize a FormData as multipart/form-data, streaming file parts from disk.
     */
    class FormDataSource : public Source {
    private:
        enum class Step {
            Header,
            Content,
            ContentEnd,
            Eof,
        };

        static constexpr std::size_t FILE_BUFFER_SIZE{ 8192 };

        FormData m_body;                                    ///< Form to serialize.
        std::size_t m_field_index{ 0 };                     ///< Current part.
        Step m_step{ Step::Header };                        ///< Position within the part.
        std::string m_buffer;                               ///< Framing text of the last chunk.
        std::ifstream m_file;                               ///< Open file of the current part.
        std::uintmax_t m_file_remaining{ 0 };               ///< Unread bytes of m_file.
        std::array<char, FILE_BUFFER_SIZE> m_file_buffer{}; ///< File read buffer.

    public:
        /**
         * @brief Take ownership of the form.
         * @param body Form to serialize; its boundary must be set.
         */
        explicit FormDataSource(FormData body) : m_body{ std::move(body) } {}

        auto Next(boost::system::error_code& ec) -> Chunk override {
            ec = {};
            if (m_field_index >= m_body.fields.size()) {
                return std::nullopt;
            }
            auto const& field{ m_body.fields[m_field_index] };
            switch (m_step) {
            case Step::Header:
                m_buffer = std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"", m_body.boundary, field.name);
                if (!field.filename.empty()) {
                    m_buffer += std::format("; filename=\"{}\"", field.filename);
                }
                m_buffer += "\r\n";
                if (!field.content_type.empty()) {
                    m_buffer += std::format("Content-Type: {}\r\n", field.content_type);
                }
                m_buffer += "\r\n";
                m_step = Step::Content;
                return std::pair{ std::string_view{ m_buffer }, true };
            case Step::Content:
                if (field.file_path) {
                    return nextFileChunk(*field.file_path, ec);
                }
                m_step = Step::ContentEnd;
                return std::pair{ std::string_view{ field.content }, true };
            case Step::ContentEnd: {
                bool const is_last{ m_field_index == m_body.fields.size() - 1 };
                m_buffer = "\r\n";
                if (is_last) {
                    m_buffer += std::format("--{}--\r\n", m_body.boundary);
                    m_step = Step::Eof;
                    ++m_field_index;
                } else {
                    m_step = Step::Header;
                    ++m_field_index;
                }
                return std::pair{ std::string_view{ m_buffer }, !is_last };
            }
            case Step::Eof:
                break;
            }
            return std::nullopt;
        }

        auto Rewind() -> bool override {
            m_file.close();
            m_file.clear();
            m_file_remaining = 0;
            m_field_index = 0;
            m_step = Step::Header;
            return true;
        }

    private:
        /**
         * @brief Produce the next chunk of a file part.
         * @param path File to stream.
         * @param ec Receives open or size errors.
         * @return File bytes, or the part trailer once the file is exhausted.
         */
        auto nextFileChunk(
            std::filesystem::path const& path,
            boost::system::error_code& ec
        ) -> Chunk {
            if (!m_file.is_open()) {
                m_file.open(path, std::ios::in | std::ios::binary);
                if (!m_file.is_open()) {
                    ec = boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
                    return std::nullopt;
                }
                std::error_code fs_ec;
                m_file_remaining = std::filesystem::file_size(path, fs_ec);
                if (fs_ec) {
                    m_file.close();
                    ec = boost::system::error_code{ fs_ec.value(), boost::system::system_category() };
                    return std::nullopt;
                }
            }
            if (m_file_remaining == 0) {
                m_file.close();
                m_step = Step::ContentEnd;
                return Next(ec);
            }
            auto const wanted{ std::min<std::uintmax_t>(FILE_BUFFER_SIZE, m_file_remaining) };
            m_file.read(m_file_buffer.data(), static_cast<std::streamsize>(wanted));
            auto const read{ static_cast<std::uintmax_t>(m_file.gcount()) };
            if (read == 0) {
                ec = http::error::short_read;
                return std::nullopt;
            }
            m_file_remaining -= read;
            if (m_file_remaining == 0) {
                m_file.close();
                m_step = Step::ContentEnd;
            }
            return std::pair{ std::string_view{ m_file_buffer.data(), static_cast<std::size_t>(read) }, true };
        }
    };

    /**
     * @brief Stream a file, a single byte range, or several ranges as multipart/byteranges.
     */
    class FileSource : public Source {
    private:
        enum class Step {
            Header,
            Content,
            ContentEnd,
            Eof,
        };

        static constexpr std::size_t BUFFER_SIZE{ 64 * 1024 };

        std::ifstream m_file;                                                      ///< Open file.
        std::uint64_t m_file_size{ 0 };                                            ///< File size in bytes.
        HttpRanges m_ranges;                                                       ///< Requested ranges; empty = whole file.
        std::string m_content_type;                                                ///< Part Content-Type for byteranges.
        std::string m_boundary;                                                    ///< Boundary for byteranges.
        std::size_t m_range_index{ 0 };                                            ///< Current range for byteranges.
        Step m_step{ Step::Header };                                               ///< Position within the current range.
        std::optional<std::uint64_t> m_pos;                                        ///< Next file offset to read.
        std::string m_framing;                                                     ///< Framing text of the last chunk.
        std::unique_ptr<char[]> m_buffer{ std::make_unique<char[]>(BUFFER_SIZE) }; ///< Read buffer.

    public:
        /**
         * @brief Take ownership of an open file.
         * @param file Open binary stream.
         * @param ranges Requested ranges; validated against the file size.
         * @param content_type Content-Type of the file, used in byteranges part headers.
         * @param boundary Boundary for byteranges; unused for zero or one range.
         */
        FileSource(
            std::ifstream file,
            HttpRanges ranges,
            std::string content_type,
            std::string boundary
        ) : m_file{ std::move(file) }, m_ranges{ std::move(ranges) }, m_content_type{ std::move(content_type) }, m_boundary{ std::move(boundary) } {
            m_file.seekg(0, std::ios::end);
            m_file_size = static_cast<std::uint64_t>(m_file.tellg());
            m_file.seekg(0, std::ios::beg);
        }

        [[nodiscard]] auto ContentLength() const -> std::optional<std::uint64_t> override {
            if (m_ranges.Empty()) {
                return m_file_size;
            }
            if (m_ranges.Size() == 1) {
                auto const& [first, last]{ m_ranges.Front() };
                return static_cast<std::uint64_t>(last - first + 1);
            }
            // multipart/byteranges framing is not precomputed; Beast falls back to chunked.
            return std::nullopt;
        }

        auto Next(boost::system::error_code& ec) -> Chunk override {
            ec = {};
            if (m_ranges.Size() <= 1) {
                std::uint64_t first{ 0 };
                std::uint64_t end{ m_file_size };
                if (!m_ranges.Empty()) {
                    first = static_cast<std::uint64_t>(m_ranges.Front().first);
                    end = static_cast<std::uint64_t>(m_ranges.Front().second) + 1;
                }
                if (m_step == Step::Eof) {
                    return std::nullopt;
                }
                auto chunk{ readSpan(first, end, ec) };
                if (!chunk || ec) {
                    return std::nullopt;
                }
                bool const more{ *m_pos < end };
                if (!more) {
                    m_step = Step::Eof;
                }
                return std::pair{ *chunk, more };
            }
            return nextByterange(ec);
        }

        auto Rewind() -> bool override {
            m_file.clear();
            m_range_index = 0;
            m_step = Step::Header;
            m_pos.reset();
            return m_file.is_open();
        }

    private:
        /**
         * @brief Read the next piece of [first, end).
         * @param first First offset of the span.
         * @param end One past the last offset.
         * @param ec Receives short_read when the file shrank.
         * @return Bytes read, or std::nullopt when the span is exhausted.
         */
        auto readSpan(
            std::uint64_t first,
            std::uint64_t end,
            boost::system::error_code& ec
        ) -> std::optional<std::string_view> {
            if (!m_pos) {
                m_pos = first;
                m_file.seekg(static_cast<std::streamoff>(first));
            }
            auto const wanted{ static_cast<std::size_t>(std::min<std::uint64_t>(BUFFER_SIZE, end - *m_pos)) };
            if (wanted == 0) {
                return std::nullopt;
            }
            m_file.read(m_buffer.get(), static_cast<std::streamsize>(wanted));
            auto const read{ static_cast<std::size_t>(m_file.gcount()) };
            if (read == 0) {
                ec = http::error::short_read;
                return std::nullopt;
            }
            *m_pos += read;
            return std::string_view{ m_buffer.get(), read };
        }

        /**
         * @brief Produce the next chunk of a multipart/byteranges body.
         * @param ec Receives short_read when the file shrank.
         * @return Next chunk, or std::nullopt when every range was sent.
         */
        auto nextByterange(boost::system::error_code& ec) -> Chunk {
            if (m_range_index >= m_ranges.Size()) {
                return std::nullopt;
            }
            auto const& [first, last]{ m_ranges.At(m_range_index) };
            switch (m_step) {
            case Step::Header:
                m_framing = std::format("--{}\r\nContent-Type: {}\r\nContent-Range: bytes {}-{}/{}\r\n\r\n", m_boundary, m_content_type, first, last, m_file_size);
                m_step = Step::Content;
                m_pos.reset();
                return std::pair{ std::string_view{ m_framing }, true };
            case Step::Content: {
                // The range is inclusive; httplib read one byte short here.
                auto chunk{ readSpan(static_cast<std::uint64_t>(first), static_cast<std::uint64_t>(last) + 1, ec) };
                if (ec) {
                    return std::nullopt;
                }
                if (!chunk) {
                    m_step = Step::ContentEnd;
                    return nextByterange(ec);
                }
                return std::pair{ *chunk, true };
            }
            case Step::ContentEnd: {
                bool const is_last{ m_range_index == m_ranges.Size() - 1 };
                m_framing = "\r\n";
                if (is_last) {
                    m_framing += std::format("--{}--\r\n", m_boundary);
                    m_step = Step::Eof;
                } else {
                    m_step = Step::Header;
                }
                ++m_range_index;
                return std::pair{ std::string_view{ m_framing }, !is_last };
            }
            case Step::Eof:
                break;
            }
            return std::nullopt;
        }
    };

} // namespace mhttp::detail
