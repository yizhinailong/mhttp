/**
 * @file multipart_parser.cppm
 * @brief Parse multipart/form-data incrementally from arbitrary body chunks.
 */
module;

#include <boost/beast/http/error.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:multipart_parser;

import std;
import :config;
import :strings;
import :http_date;
import :form_data;

namespace mhttp::detail {

    /**
     * @brief Length of the longest suffix of `text` that is a prefix of `a` or `b`.
     *
     * Bytes that may start a boundary delimiter split across chunks are kept pending instead
     * of being committed as content.
     *
     * @param text Buffered bytes.
     * @param a First delimiter.
     * @param b Second delimiter.
     * @return Number of trailing bytes to keep.
     */
    auto longest_suffix_prefix(
        std::string_view text,
        std::string_view a,
        std::string_view b
    ) -> std::size_t {
        auto const limit{ std::min(text.size(), std::max(a.size(), b.size())) };
        for (std::size_t k{ limit }; k >= 1; --k) {
            auto const tail{ text.substr(text.size() - k) };
            if ((a.size() >= k && a.substr(0, k) == tail) || (b.size() >= k && b.substr(0, k) == tail)) {
                return k;
            }
        }
        return 0;
    }

    /**
     * @brief Parse the parameters of a Content-Disposition value after "form-data;".
     * @param header Parameter list such as `name="a"; filename="b.txt"`.
     * @return Key/value views into `header`; quoted values exclude the quotes.
     */
    auto parse_content_disposition(
        std::string_view header
    ) -> std::vector<std::pair<std::string_view, std::string_view>> {
        std::vector<std::pair<std::string_view, std::string_view>> results;
        std::size_t pos{ 0 };
        while (pos < header.size()) {
            auto const eq{ header.find('=', pos) };
            if (eq == std::string_view::npos) {
                break;
            }
            auto const key{ trim(header.substr(pos, eq - pos)) };
            pos = eq + 1;

            std::string_view value;
            if (pos < header.size() && header[pos] == '"') {
                ++pos;
                auto end{ pos };
                bool escape{ false };
                while (end < header.size()) {
                    if (header[end] == '\\' && !escape) {
                        escape = true;
                    } else if (header[end] == '"' && !escape) {
                        break;
                    } else {
                        escape = false;
                    }
                    ++end;
                }
                value = header.substr(pos, end - pos);
                pos = end < header.size() ? end + 1 : end;
            } else {
                auto end{ header.find(';', pos) };
                if (end == std::string_view::npos) {
                    end = header.size();
                }
                value = trim(header.substr(pos, end - pos));
                pos = end;
            }
            results.emplace_back(key, value);

            if (pos < header.size() && header[pos] == ';') {
                ++pos;
            }
            while (pos < header.size() && is_space(header[pos])) {
                ++pos;
            }
        }
        return results;
    }

    /**
     * @brief Incremental multipart/form-data parser (port of httplib::body::multipart_parser).
     */
    class MultipartParser {
    private:
        enum class Step {
            BoundaryLine,
            BoundaryHeader,
            BoundaryContent,
            Eof,
        };

        FormData m_body;                   ///< Parsed parts.
        std::string m_content_type;        ///< Content-Type carrying the boundary.
        std::string m_boundary_line;       ///< "--boundary\r\n".
        std::string m_boundary_line_last;  ///< "--boundary--\r\n".
        std::string m_delim_field;         ///< "\r\n--boundary\r\n".
        std::string m_delim_final;         ///< "\r\n--boundary--\r\n".
        Step m_step{ Step::BoundaryLine }; ///< Parser position.
        FormData::Field m_field;           ///< Part being parsed.
        std::string m_pending;             ///< Bytes that may be a split delimiter.
        std::string m_combined;            ///< m_pending plus the next chunk.
        std::ofstream m_file;              ///< Open file for the current file part.
        std::filesystem::path m_file_path; ///< Path of m_file.
        std::uint64_t m_field_bytes{ 0 };  ///< Content bytes of the current part.

    public:
        /**
         * @brief Create a parser for one body.
         * @param content_type Request Content-Type including the boundary parameter.
         * @param params Limits and file handling for the parsed form.
         */
        MultipartParser(
            std::string content_type,
            FormData::Param params
        ) : m_content_type{ std::move(content_type) } {
            m_body.params = std::move(params);
        }

        /**
         * @brief Extract the boundary and reset the state machine.
         * @param ec Receives http::error::bad_field when no boundary is present.
         */
        auto Reset(boost::system::error_code& ec) -> void {
            ec = {};
            std::string boundary;
            for (auto const part : split(m_content_type, ";")) {
                auto const trimmed{ trim(part) };
                if (!trimmed.starts_with("boundary")) {
                    continue;
                }
                auto const pair{ split(trimmed, "=") };
                if (pair.size() == 2) {
                    boundary = std::string{ trim(pair[1]) };
                }
            }
            // RFC 2046 allows a quoted boundary parameter.
            if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
                boundary = boundary.substr(1, boundary.size() - 2);
            }
            if (boundary.empty()) {
                ec = http::error::bad_field;
                return;
            }
            m_body.boundary = boundary;
            m_boundary_line = "--" + boundary + "\r\n";
            m_boundary_line_last = "--" + boundary + "--\r\n";
            m_delim_field = "\r\n--" + boundary + "\r\n";
            m_delim_final = "\r\n--" + boundary + "--\r\n";
            m_step = Step::BoundaryLine;
            m_pending.clear();
            m_combined.clear();
            m_field_bytes = 0;
            if (m_file.is_open()) {
                m_file.close();
            }
        }

        /**
         * @brief Feed the next body chunk.
         * @param chunk Body bytes.
         * @param ec Receives unexpected_body, body_limit, or file errors.
         */
        auto Put(std::string_view chunk, boost::system::error_code& ec) -> void {
            ec = {};
            std::string_view sv{ chunk };
            if (!m_pending.empty()) {
                m_combined.clear();
                m_combined.reserve(m_pending.size() + chunk.size());
                m_combined.append(m_pending);
                m_combined.append(chunk);
                m_pending.clear();
                sv = m_combined;
            }

            if (m_step == Step::Eof) {
                if (!sv.empty()) {
                    ec = http::error::unexpected_body;
                }
                return;
            }

            bool need_more_data{ false };
            while (!sv.empty() && !ec && !need_more_data) {
                switch (m_step) {
                case Step::BoundaryLine:
                    if (sv.starts_with(m_boundary_line)) {
                        sv.remove_prefix(m_boundary_line.size());
                        m_step = Step::BoundaryHeader;
                    } else if (sv.starts_with(m_boundary_line_last)) {
                        sv.remove_prefix(m_boundary_line_last.size());
                        m_step = Step::Eof;
                    } else if (std::string_view{ m_boundary_line }.starts_with(sv) || std::string_view{ m_boundary_line_last }.starts_with(sv)) {
                        need_more_data = true;
                    } else {
                        ec = http::error::unexpected_body;
                    }
                    break;
                case Step::BoundaryHeader:
                    need_more_data = !parsePartHeader(sv, ec);
                    break;
                case Step::BoundaryContent:
                    need_more_data = !parsePartContent(sv, ec);
                    break;
                case Step::Eof:
                    ec = http::error::unexpected_body;
                    break;
                }
            }
            if (ec) {
                return;
            }
            if (need_more_data) {
                m_pending.assign(sv);
            }
        }

        /**
         * @brief Finish the body.
         * @param ec Receives http::error::partial_message when the closing boundary is missing.
         */
        auto Finish(boost::system::error_code& ec) -> void {
            ec = {};
            if (m_file.is_open()) {
                m_file.close();
            }
            if (m_step != Step::Eof) {
                ec = http::error::partial_message;
            }
        }

        /**
         * @brief Move the parsed form out.
         * @return Parsed form.
         */
        auto Take() -> FormData {
            return std::move(m_body);
        }

    private:
        /**
         * @brief Parse one part header block.
         * @param sv Remaining input; advanced past the header on success.
         * @param ec Receives unexpected_body on malformed headers.
         * @return False when more input is needed.
         */
        auto parsePartHeader(std::string_view& sv, boost::system::error_code& ec) -> bool {
            auto const end{ sv.find("\r\n\r\n") };
            if (end == std::string_view::npos) {
                return false;
            }
            FormData::Field field;
            for (auto const line : split(sv.substr(0, end + 4), "\r\n")) {
                if (line.empty()) {
                    continue;
                }
                auto const colon{ line.find(':') };
                if (colon == std::string_view::npos) {
                    ec = http::error::unexpected_body;
                    return true;
                }
                auto const name{ trim(line.substr(0, colon)) };
                auto value{ trim(line.substr(colon + 1)) };
                if (iequals(name, "Content-Disposition")) {
                    auto const semi{ value.find(';') };
                    if (semi == std::string_view::npos || trim(value.substr(0, semi)) != "form-data") {
                        ec = http::error::unexpected_body;
                        return true;
                    }
                    value.remove_prefix(semi + 1);
                    for (auto const& [key, param] : parse_content_disposition(value)) {
                        if (key == "name") {
                            field.name = param;
                        } else if (key == "filename") {
                            field.filename = param;
                        }
                    }
                } else if (iequals(name, "Content-Type")) {
                    field.content_type = value;
                }
            }
            m_field = std::move(field);
            m_field_bytes = 0;
            sv.remove_prefix(end + 4);
            m_step = Step::BoundaryContent;
            return true;
        }

        /**
         * @brief Consume part content up to the next delimiter.
         * @param sv Remaining input; advanced past what was consumed.
         * @param ec Receives body_limit or file errors.
         * @return False when the tail may be a split delimiter and more input is needed.
         */
        auto parsePartContent(std::string_view& sv, boost::system::error_code& ec) -> bool {
            auto const pos_field{ sv.find(m_delim_field) };
            auto const pos_final{ sv.find(m_delim_final) };
            auto pos{ std::string_view::npos };
            bool is_final{ false };
            if (pos_field != std::string_view::npos && (pos_final == std::string_view::npos || pos_field <= pos_final)) {
                pos = pos_field;
            } else if (pos_final != std::string_view::npos) {
                pos = pos_final;
                is_final = true;
            }

            bool const save_to_file{ !m_field.filename.empty() && !m_body.params.save_dir.empty() };
            if (pos != std::string_view::npos) {
                commitContent(sv.substr(0, pos), save_to_file, ec);
                if (ec) {
                    return true;
                }
                if (save_to_file && m_file.is_open()) {
                    m_file.close();
                    m_field.file_path = m_file_path;
                }
                sv.remove_prefix(pos + (is_final ? m_delim_final.size() : m_delim_field.size()));
                if (m_body.params.max_fields != 0 && m_body.fields.size() >= m_body.params.max_fields) {
                    ec = http::error::body_limit;
                    return true;
                }
                m_body.fields.push_back(std::move(m_field));
                m_step = is_final ? Step::Eof : Step::BoundaryHeader;
                return true;
            }

            auto const keep{ longest_suffix_prefix(sv, m_delim_field, m_delim_final) };
            commitContent(sv.substr(0, sv.size() - keep), save_to_file, ec);
            sv.remove_prefix(sv.size() - keep);
            return keep == 0;
        }

        /**
         * @brief Append content to the current part, enforcing max_file_size for every part.
         * @param data Content bytes.
         * @param save_to_file Write to save_dir instead of memory.
         * @param ec Receives body_limit or file errors.
         */
        auto commitContent(
            std::string_view data,
            bool save_to_file,
            boost::system::error_code& ec
        ) -> void {
            if (data.empty()) {
                return;
            }
            // Without this, the default (in-memory) configuration would bypass max_file_size.
            m_field_bytes += data.size();
            if (m_body.params.max_file_size != 0 && m_field_bytes > m_body.params.max_file_size) {
                abortFile();
                ec = http::error::body_limit;
                return;
            }
            if (!save_to_file) {
                m_field.content.append(data);
                return;
            }
            writeContent(data, ec);
        }

        /**
         * @brief Write file content, opening a uniquely named file inside save_dir first.
         * @param data Content bytes.
         * @param ec Receives body_limit when the path escapes save_dir, io_error on write failure.
         */
        auto writeContent(std::string_view data, boost::system::error_code& ec) -> void {
            if (!m_file.is_open()) {
                // Client file names are only unique within one request; a random prefix keeps
                // concurrent uploads (and same-named parts) from overwriting each other.
                auto safe_name{ std::filesystem::path{ m_field.filename }.filename().string() };
                if (safe_name.empty() || safe_name == "." || safe_name == "..") {
                    safe_name = "upload";
                }
                auto const candidate{ m_body.params.save_dir / (random_hex(16) + "_" + safe_name) };
                auto const canonical_dir{ std::filesystem::weakly_canonical(m_body.params.save_dir).string() };
                auto const canonical_file{ std::filesystem::weakly_canonical(candidate).string() };
                if (!canonical_file.starts_with(canonical_dir)) {
                    ec = http::error::body_limit;
                    return;
                }
                m_file_path = candidate;
                m_file.open(m_file_path, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!m_file.is_open()) {
                    ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
                    return;
                }
            }
            m_file.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!m_file) {
                ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
        }

        /**
         * @brief Close and delete a half-written file after a limit was hit.
         */
        auto abortFile() -> void {
            if (m_file.is_open()) {
                m_file.close();
                std::error_code rm_ec;
                std::filesystem::remove(m_file_path, rm_ec);
            }
        }
    };

} // namespace mhttp::detail
