/**
 * @file form_data.cppm
 * @brief Parse bounded in-memory multipart forms into independently owned, ordered fields and files.
 */
export module mhttp:core.form_data;

import std;
import :utils.string;
export import :core.headers;

export namespace mhttp {

    /**
     * @brief Own one multipart field or file without borrowing HTTP input or filesystem resources.
     * @note Filename is advisory metadata; applications choose and validate any filesystem destination.
     */
    struct FormPart {
        std::string name; ///< Case-sensitive field name, including an explicitly empty name.
        std::string content; ///< Owned binary bytes, including NULs and CRLFs.
        std::optional<std::string> filename; ///< File metadata; nullopt denotes an ordinary field, empty denotes an empty filename.
        std::string content_type; ///< Complete optional media type, without charset conversion.
    };

    /**
     * @brief Bound multipart admission independently of the HTTP message's existing byte limit.
     */
    struct MultipartLimits {
        std::size_t max_parts{ 128 }; ///< Combined field/file count, including repeated names.
        std::size_t part_bytes{ 8 * 1024 * 1024 }; ///< Content bytes per part, applying equally to files and ordinary fields.
        std::size_t body_bytes{ 8 * 1024 * 1024 }; ///< Complete representation, including preamble, delimiters, headers, and epilogue.
        std::size_t part_header_bytes{ 8 * 1024 }; ///< Bytes per part's header block, including its terminating CRLF CRLF.
        std::size_t part_header_fields{ 32 }; ///< Header count per part, including ignored fields and duplicates.
    };

    /**
     * @brief Own a complete encoded multipart representation and its matching Content-Type declaration.
     */
    struct MultipartBody {
        std::string body; ///< Complete binary representation, independent of the original form.
        std::string content_type; ///< Matching media type with an explicitly quoted boundary.
    };

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Retain a borrowed primary value and owned, unescaped, case-insensitive parameters during parsing.
     */
    struct MultipartParameters {
        std::string_view token; ///< Borrowed until the parameter parser's caller returns.
        std::vector<std::pair<std::string, std::string>> values; ///< Lowercase names and owned quoted-pair-decoded values.
    };

    /**
     * @brief Parse bounded MIME parameters without splitting semicolons or escaped quotes inside quoted values.
     * @param input Borrowed field value, retained by the caller throughout use of the result.
     * @return Borrowed primary token with owned parameters, or InvalidHeaders for malformed/duplicate parameters.
     */
    auto parse_multipart_parameters(std::string_view input) -> Result<MultipartParameters> {
        input = utils::trim_ows(input);
        if (input.size() > 64 * 1024 || !utils::is_field_value(input)) {
            return std::unexpected{ Error::InvalidHeaders };
        }
        auto separator = input.find(';');
        MultipartParameters result{ .token = utils::trim_ows(input.substr(0, separator)) };
        if (result.token.empty()) { return std::unexpected{ Error::InvalidHeaders }; }
        auto offset = separator;
        while (offset != std::string_view::npos && offset < input.size()) {
            ++offset;
            while (offset < input.size() && utils::is_ows(input[offset])) { ++offset; }
            auto equal = input.find('=', offset);
            if (equal == std::string_view::npos || result.values.size() == 32) {
                return std::unexpected{ Error::InvalidHeaders };
            }
            auto key = utils::trim_ows(input.substr(offset, equal - offset));
            if (!utils::is_field_name(key)) { return std::unexpected{ Error::InvalidHeaders }; }
            for (auto const& [name, value] : result.values) {
                if (utils::iequals(key, name)) { return std::unexpected{ Error::InvalidHeaders }; }
            }
            offset = equal + 1;
            while (offset < input.size() && utils::is_ows(input[offset])) { ++offset; }
            std::string value;
            if (offset < input.size() && input[offset] == '"') {
                ++offset;
                bool closed{};
                while (offset < input.size()) {
                    auto byte = input[offset++];
                    if (byte == '"') { closed = true; break; }
                    if (byte == '\\') {
                        if (offset == input.size()) { return std::unexpected{ Error::InvalidHeaders }; }
                        byte = input[offset++];
                    }
                    value += byte;
                }
                if (!closed) { return std::unexpected{ Error::InvalidHeaders }; }
                while (offset < input.size() && utils::is_ows(input[offset])) { ++offset; }
                if (offset < input.size() && input[offset] != ';') { return std::unexpected{ Error::InvalidHeaders }; }
            } else {
                auto end = input.find(';', offset);
                auto token = utils::trim_ows(input.substr(offset, end - offset));
                if (!utils::is_field_name(token)) { return std::unexpected{ Error::InvalidHeaders }; }
                value = token;
                offset = end;
            }
            result.values.emplace_back(utils::ToLower(key), std::move(value));
        }
        return result;
    }

    /**
     * @brief Borrow a parsed parameter without treating an explicitly empty value as absence.
     * @param parameters Borrowed parameter storage.
     * @param name Lowercase parameter name.
     * @return Borrowed value, absent when not declared.
     */
    auto multipart_parameter(
        MultipartParameters const& parameters,
        std::string_view name
    ) -> std::optional<std::string_view> {
        for (auto const& [key, value] : parameters.values) {
            if (key == name) { return value; }
        }
        return {};
    }

    /**
     * @brief Recognize the RFC 2046 boundary alphabet and its length/trailing-space rules.
     * @param boundary Borrowed unquoted value.
     * @return Whether this is a boundary of one to seventy ASCII bytes.
     */
    auto valid_multipart_boundary(std::string_view boundary) noexcept -> bool {
        if (boundary.empty() || boundary.size() > 70 || boundary.back() == ' ') { return false; }
        return std::ranges::all_of(boundary, [](unsigned char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z') || std::string_view{ "'()+_,-./:=? " }.contains(static_cast<char>(byte));
        });
    }

    /**
     * @brief Validate a multipart media declaration and extract one independently owned boundary.
     * @param content_type Borrowed complete Content-Type value.
     * @return Owned boundary, InvalidRequest for another type/missing/invalid boundary, or InvalidHeaders for syntax.
     */
    auto parse_multipart_boundary(std::string_view content_type) -> Result<std::string> {
        if (utils::trim_ows(content_type).empty()) { return std::unexpected{ Error::InvalidRequest }; }
        auto parameters = parse_multipart_parameters(content_type);
        if (!parameters) { return std::unexpected{ parameters.error() }; }
        auto boundary = multipart_parameter(*parameters, "boundary");
        if (!utils::iequals(parameters->token, "multipart/form-data") || !boundary || !valid_multipart_boundary(*boundary)) {
            return std::unexpected{ Error::InvalidRequest };
        }
        return std::string{ *boundary };
    }

    /**
     * @brief Describe one complete delimiter line without borrowing or copying body bytes.
     */
    struct MultipartDelimiter {
        std::size_t position; ///< First byte excluded from the previous part, including the framing CRLF.
        std::size_t next; ///< First byte after the delimiter line.
        bool closing; ///< Final delimiter, after which epilogue bytes are ignored.
    };

    /**
     * @brief Accept transport padding and exact line endings after a candidate boundary token.
     * @param body Borrowed complete representation.
     * @param token Borrowed double-dash boundary token.
     * @param position Candidate token offset.
     * @param end Previous content end, excluding framing CRLF.
     * @return Complete delimiter, absent for a boundary-like content prefix.
     */
    auto read_multipart_delimiter(
        std::string_view body,
        std::string_view token,
        std::size_t position,
        std::size_t end
    ) -> std::optional<MultipartDelimiter> {
        auto offset = position + token.size();
        bool closing = body.substr(offset, 2) == "--";
        if (closing) { offset += 2; }
        while (offset < body.size() && utils::is_ows(body[offset])) { ++offset; }
        if (closing && offset == body.size()) { return MultipartDelimiter{ end, offset, true }; }
        if (body.substr(offset, 2) != "\r\n") { return {}; }
        return MultipartDelimiter{ end, offset + 2, closing };
    }

    /**
     * @brief Scan forward for a complete delimiter using a fixed, at-most-74-byte search pattern.
     * @param body Borrowed complete representation.
     * @param token Double-dash boundary token.
     * @param offset First unsearched byte.
     * @param first Permit an initial boundary at body offset zero, otherwise require preceding CRLF.
     * @return Next delimiter, absent when the remaining representation is malformed/truncated.
     * @note Search positions only advance; preamble and missing-boundary scanning do not copy suffixes.
     */
    auto find_multipart_delimiter(
        std::string_view body,
        std::string_view token,
        std::size_t offset,
        bool first = false
    ) -> std::optional<MultipartDelimiter> {
        if (first && body.starts_with(token)) {
            if (auto delimiter = read_multipart_delimiter(body, token, 0, 0)) { return delimiter; }
        }
        auto marker = "\r\n" + std::string{ token };
        while ((offset = body.find(marker, offset)) != std::string_view::npos) {
            if (auto delimiter = read_multipart_delimiter(body, token, offset + 2, offset)) { return delimiter; }
            offset += 2;
        }
        return {};
    }

    /**
     * @brief Validate every bounded part header, then own its form-data disposition and optional media type.
     * @param block Borrowed header lines without the final CRLF CRLF.
     * @param limits Per-part field-count admission.
     * @return Owned metadata without content, or InvalidHeaders for malformed/ambiguous headers.
     */
    auto parse_multipart_part(
        std::string_view block,
        MultipartLimits const& limits
    ) -> Result<FormPart> {
        Headers headers;
        std::size_t offset{}, count{};
        while (offset < block.size()) {
            auto end = block.find("\r\n", offset);
            if (end == std::string_view::npos) { end = block.size(); }
            auto line = block.substr(offset, end - offset);
            auto colon = line.find(':');
            if (++count > limits.part_header_fields || line.empty() || utils::is_ows(line.front())
                || colon == std::string_view::npos || !headers.Add(line.substr(0, colon), utils::trim_ows(line.substr(colon + 1)))) {
                return std::unexpected{ Error::InvalidHeaders };
            }
            offset = end == block.size() ? end : end + 2;
        }
        if (headers.Count("Content-Disposition") != 1 || headers.Count("Content-Type") > 1) {
            return std::unexpected{ Error::InvalidHeaders };
        }
        auto disposition = parse_multipart_parameters(headers.Get("Content-Disposition"));
        if (!disposition || !utils::iequals(disposition->token, "form-data")) {
            return std::unexpected{ Error::InvalidHeaders };
        }
        auto name = multipart_parameter(*disposition, "name");
        if (!name) { return std::unexpected{ Error::InvalidHeaders }; }
        FormPart part{ .name = std::string{ *name } };
        if (auto filename = multipart_parameter(*disposition, "filename")) { part.filename = std::string{ *filename }; }
        part.content_type = headers.Get("Content-Type");
        if (headers.Has("Content-Type")) {
            auto parameters = parse_multipart_parameters(part.content_type);
            if (!parameters) { return std::unexpected{ Error::InvalidHeaders }; }
            auto type = parameters->token;
            auto slash = type.find('/');
            if (slash == std::string_view::npos || !utils::is_field_name(type.substr(0, slash))
                || !utils::is_field_name(type.substr(slash + 1))) { return std::unexpected{ Error::InvalidHeaders }; }
        }
        return part;
    }

    /**
     * @brief Own admitted part content while preserving wire order and treating boundary-like prefixes as data.
     * @param body Borrowed complete representation; all copies obey admission limits.
     * @param boundary Borrowed validated boundary.
     * @param limits Whole-body, per-part, and header bounds.
     * @return Owned parts or a recoverable syntax/header/size error.
     */
    auto parse_multipart_parts(
        std::string_view body,
        std::string_view boundary,
        MultipartLimits limits
    ) -> Result<std::vector<FormPart>> {
        if (body.size() > limits.body_bytes) { return std::unexpected{ Error::BodyTooLarge }; }
        auto token = "--" + std::string{ boundary };
        auto delimiter = find_multipart_delimiter(body, token, 0, true);
        if (!delimiter) { return std::unexpected{ Error::InvalidRequest }; }
        std::vector<FormPart> parts;
        while (!delimiter->closing) {
            if (parts.size() == limits.max_parts) { return std::unexpected{ Error::BodyTooLarge }; }
            auto offset = delimiter->next;
            auto block = body.substr(offset, std::min(body.size() - offset, limits.part_header_bytes));
            auto end = block.find("\r\n\r\n");
            if (end == std::string_view::npos) { return std::unexpected{ Error::InvalidHeaders }; }
            auto part = parse_multipart_part(block.substr(0, end), limits);
            if (!part) { return std::unexpected{ part.error() }; }
            offset += end + 4;
            delimiter = find_multipart_delimiter(body, token, offset);
            if (!delimiter) { return std::unexpected{ Error::InvalidRequest }; }
            auto bytes = delimiter->position - offset;
            if (bytes > limits.part_bytes) { return std::unexpected{ Error::BodyTooLarge }; }
            part->content = body.substr(offset, bytes);
            parts.push_back(std::move(*part));
        }
        return parts;
    }

    /**
     * @brief Escape quoted metadata without allowing controls or quotes to create extra MIME headers.
     * @param value Borrowed field name or filename, including arbitrary control bytes.
     * @param limit Maximum encoded parameter bytes before allocating further output.
     * @return Owned safe parameter text or InvalidHeaders for excessive encoded metadata.
     * @note Quotes/controls use literal percent escapes; backslashes use MIME quoted-pairs.
     */
    auto quote_multipart_parameter(
        std::string_view value,
        std::size_t limit
    ) -> Result<std::string> {
        if (value.size() > limit) { return std::unexpected{ Error::InvalidHeaders }; }
        constexpr std::string_view HEX{ "0123456789ABCDEF" };
        std::string result;
        for (unsigned char byte : value) {
            auto count = byte == '"' || byte < 0x20 || byte == 0x7f ? 3U : byte == '\\' ? 2U : 1U;
            if (count > limit - result.size()) { return std::unexpected{ Error::InvalidHeaders }; }
            if (count == 3) { result += '%'; result += HEX[byte >> 4]; result += HEX[byte & 15]; }
            else { if (byte == '\\') { result += '\\'; } result += static_cast<char>(byte); }
        }
        return result;
    }

    /**
     * @brief Own validated disposition/media headers while admitting their full encoded size and field count.
     * @param part Borrowed field/file metadata, never retained.
     * @param limits Header allocation and count bounds.
     * @return Complete header block or InvalidHeaders for invalid/oversized metadata.
     */
    auto encode_multipart_headers(
        FormPart const& part,
        MultipartLimits limits
    ) -> Result<std::string> {
        auto name = quote_multipart_parameter(part.name, limits.part_header_bytes);
        if (!name) { return std::unexpected{ name.error() }; }
        std::string headers = "Content-Disposition: form-data; name=\"" + *name + '"';
        if (part.filename) {
            auto filename = quote_multipart_parameter(*part.filename, limits.part_header_bytes);
            if (!filename) { return std::unexpected{ filename.error() }; }
            headers += "; filename=\"" + *filename + '"';
        }
        headers += "\r\n";
        std::string_view type = part.content_type;
        if (type.empty() && part.filename) { type = "application/octet-stream"; }
        if (!type.empty()) {
            if (type.size() > limits.part_header_bytes || !utils::is_field_value(type)) {
                return std::unexpected{ Error::InvalidHeaders };
            }
            headers += "Content-Type: " + std::string{ type } + "\r\n";
        }
        headers += "\r\n";
        if (headers.size() > limits.part_header_bytes) { return std::unexpected{ Error::InvalidHeaders }; }
        auto valid = parse_multipart_part(std::string_view{ headers }.substr(0, headers.size() - 4), limits);
        if (!valid) { return std::unexpected{ valid.error() }; }
        return headers;
    }

    /**
     * @brief Refuse a boundary token at the beginning of any content line, including ambiguous token prefixes.
     * @param parts Borrowed admitted binary parts.
     * @param boundary Borrowed valid candidate.
     * @return Whether any part could be confused with this candidate's delimiter.
     */
    auto multipart_boundary_conflicts(
        std::vector<FormPart> const& parts,
        std::string_view boundary
    ) -> bool {
        auto token = "--" + std::string{ boundary };
        auto marker = "\r\n" + token;
        for (auto const& part : parts) {
            if (part.content.starts_with(token) || part.content.contains(marker)) { return true; }
        }
        return false;
    }

    /**
     * @brief Generate a process-local candidate without blocking entropy sources; content checks establish safety.
     * @return ASCII boundary containing a steady-clock count and a synchronized monotonic sequence.
     */
    auto make_multipart_boundary() -> std::string {
        static std::atomic<std::uint64_t> s_sequence{};
        auto ticks = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return std::format("mhttp-{:016x}-{:016x}", ticks, s_sequence.fetch_add(1, std::memory_order_relaxed));
    }

    /**
     * @brief Materialize one bounded multipart body before changing a message or issuing asynchronous work.
     * @param parts Borrowed owned form values, consumed synchronously.
     * @param requested_boundary Empty selects a checked generated candidate; explicit conflicts are errors.
     * @param limits Complete output, part, count, and metadata admission bounds.
     * @return Owned encoded body/media type or a recoverable metadata, boundary, or size error.
     */
    auto encode_multipart_parts(
        std::vector<FormPart> const& parts,
        std::string_view requested_boundary,
        MultipartLimits limits
    ) -> Result<MultipartBody> {
        if (!requested_boundary.empty() && !valid_multipart_boundary(requested_boundary)) {
            return std::unexpected{ Error::InvalidRequest };
        }
        if (parts.size() > limits.max_parts) { return std::unexpected{ Error::BodyTooLarge }; }
        std::size_t content_bytes{};
        std::vector<std::string> headers;
        for (auto const& part : parts) {
            if (part.content.size() > limits.part_bytes || part.content.size() > limits.body_bytes - content_bytes) {
                return std::unexpected{ Error::BodyTooLarge };
            }
            content_bytes += part.content.size();
            auto encoded = encode_multipart_headers(part, limits);
            if (!encoded) { return std::unexpected{ encoded.error() }; }
            headers.push_back(std::move(*encoded));
        }
        std::string boundary{ requested_boundary };
        if (!boundary.empty()) {
            if (multipart_boundary_conflicts(parts, boundary)) {
                return std::unexpected{ Error::InvalidRequest };
            }
        } else {
            for (unsigned attempt{}; attempt < 16; ++attempt) {
                boundary = make_multipart_boundary();
                if (!multipart_boundary_conflicts(parts, boundary)) { break; }
                boundary.clear();
            }
            if (boundary.empty()) { return std::unexpected{ Error::InvalidRequest }; }
        }
        MultipartBody result{ .content_type = "multipart/form-data; boundary=\"" + boundary + '"' };
        auto append = [&](std::string_view bytes) {
            if (bytes.size() > limits.body_bytes - result.body.size()) { return false; }
            result.body.append(bytes);
            return true;
        };
        for (std::size_t index{}; index < parts.size(); ++index) {
            if (!append("--" + boundary + "\r\n") || !append(headers[index])
                || !append(parts[index].content) || !append("\r\n")) { return std::unexpected{ Error::BodyTooLarge }; }
        }
        if (!append("--" + boundary + "--\r\n")) { return std::unexpected{ Error::BodyTooLarge }; }
        return result;
    }

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Own ordered multipart fields/files without caching views into an HTTP body.
     * @note Concurrent mutation requires synchronization. Parts/Get borrow until mutation or destruction.
     */
    class FormData {
    private:
        std::vector<FormPart> m_parts; ///< Owned fields and files in insertion/wire order.

    public:
        /**
         * @brief Parse complete multipart bytes using one media declaration and explicit allocation bounds.
         * @param body Borrowed bytes, not retained after this call.
         * @param content_type Complete multipart/form-data declaration, with required boundary.
         * @param limits Admission bounds in addition to any HTTP body limit.
         * @return Independent form, InvalidRequest for syntax/type/boundary, InvalidHeaders for part metadata,
         *         or BodyTooLarge for whole-body, part-byte, or part-count admission; allocation errors propagate.
         */
        [[nodiscard]] static auto Parse(
            std::string_view body,
            std::string_view content_type,
            MultipartLimits limits = {}
        ) -> Result<FormData> {
            auto boundary = ParseBoundary(content_type);
            if (!boundary) { return std::unexpected{ boundary.error() }; }
            auto parts = detail::parse_multipart_parts(body, *boundary, limits);
            if (!parts) { return std::unexpected{ parts.error() }; }
            FormData form;
            form.m_parts = std::move(*parts);
            return form;
        }

        /**
         * @brief Validate MIME parameters and extract a one-to-seventy-byte boundary without borrowed state.
         * @param content_type Complete declaration, including optional charset/quoted values.
         * @return Owned boundary, InvalidRequest for type/missing/invalid boundary, or InvalidHeaders for syntax.
         */
        [[nodiscard]] static auto ParseBoundary(
            std::string_view content_type
        ) -> Result<std::string> {
            return detail::parse_multipart_boundary(content_type);
        }

        /**
         * @brief Encode owned binary parts with matching metadata and a conflict-checked boundary.
         * @param boundary Optional explicit boundary; empty generates a candidate checked against every content line.
         * @param limits Output/count/content/header bounds applied before returning any bytes.
         * @return Complete owned representation, InvalidRequest for invalid/conflicting boundaries,
         *         InvalidHeaders for invalid/excessive metadata, or BodyTooLarge for output/part/count bounds.
         * @note Input is consumed synchronously, allocation failures propagate, and file parts without a type
         *       use application/octet-stream. Metadata percent escapes remain literal when parsed again.
         */
        [[nodiscard]] auto Encode(
            std::string_view boundary = {},
            MultipartLimits limits = {}
        ) const -> Result<MultipartBody> {
            return detail::encode_multipart_parts(m_parts, boundary, limits);
        }

        /**
         * @brief Append an owned field or file; encoding applies byte/count/header validation later.
         * @param part Owned metadata and binary bytes, copied or moved before storage changes.
         */
        auto Add(FormPart part) -> void { m_parts.push_back(std::move(part)); }

        /**
         * @brief Borrow all fields/files in original order, including duplicate names.
         * @return Immutable parts, invalidated by mutation or destruction.
         */
        [[nodiscard]] auto Parts() const noexcept -> std::vector<FormPart> const& { return m_parts; }

        /**
         * @brief Borrow the first case-sensitive field/file match without merging duplicate values.
         * @param name Borrowed lookup name.
         * @return Borrowed part pointer, or nullptr when absent.
         */
        [[nodiscard]] auto Get(std::string_view name) const noexcept -> FormPart const* {
            for (auto const& part : m_parts) { if (part.name == name) { return &part; } }
            return nullptr;
        }

        /**
         * @brief Count all parts with the same case-sensitive name.
         * @param name Borrowed lookup name.
         * @return Field/file count, preserving the distinction from an absent name.
         */
        [[nodiscard]] auto Count(std::string_view name) const noexcept -> std::size_t {
            return std::ranges::count(m_parts, name, &FormPart::name);
        }

        /**
         * @brief Inspect the combined number of fields and files.
         * @return Current part count.
         */
        [[nodiscard]] auto Size() const noexcept -> std::size_t { return m_parts.size(); }
    };

} // namespace mhttp
