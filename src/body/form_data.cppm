/**
 * @file form_data.cppm
 * @brief Provide the multipart/form-data value type.
 */
export module mhttp:form_data;

import std;

export namespace mhttp {

    /**
     * @brief Parsed or outgoing multipart/form-data fields.
     *
     * Move-only: one FormData is the single holder of its fields' file_path, so ownership of
     * those files is never ambiguous. With params.remove_uploaded_files the destructor removes
     * every field's file_path.
     */
    class FormData {
    public:
        /**
         * @brief One form part.
         */
        struct Field {
            std::string name;                               ///< Field name from Content-Disposition.
            std::string filename;                           ///< Client file name; empty for plain fields.
            std::string content_type;                       ///< Part Content-Type; may be empty.
            std::string content;                            ///< In-memory content.
            std::optional<std::filesystem::path> file_path; ///< Content stored on disk instead of memory.

            /**
             * @brief Report whether the part carries content.
             * @return True for non-empty content or a file path.
             */
            [[nodiscard]] auto HasData() const -> bool {
                return !content.empty() || file_path.has_value();
            }

            /**
             * @brief Report whether the part is a file upload.
             * @return True when a filename was given.
             */
            [[nodiscard]] auto IsFile() const -> bool {
                return !filename.empty();
            }
        };

        /**
         * @brief Parsing limits and file handling.
         */
        struct Param {
            std::filesystem::path save_dir;      ///< Directory for file parts; empty keeps them in memory.
            std::uint64_t max_file_size{ 0 };    ///< Per-part content limit for files and fields; 0 = unlimited.
            std::size_t max_fields{ 128 };       ///< Maximum number of parts; 0 = unlimited.
            bool remove_uploaded_files{ false }; ///< Remove every file_path on destruction.
        };

        std::vector<Field> fields; ///< Parts in wire order.
        std::string boundary;      ///< Multipart boundary without the leading "--".
        Param params;              ///< Limits used while parsing and the cleanup policy.

        FormData() = default;
        FormData(FormData const&) = delete;
        auto operator=(FormData const&) -> FormData& = delete;

        /**
         * @brief Take over another form's parts and file ownership.
         * @param other Source; it no longer removes files.
         */
        FormData(
            FormData&& other
        ) noexcept : fields{ std::move(other.fields) }, boundary{ std::move(other.boundary) }, params{ other.params } {
            other.params.remove_uploaded_files = false;
        }

        /**
         * @brief Take over another form's parts and file ownership.
         * @param other Source; it no longer removes files.
         * @return This form.
         */
        auto operator=(FormData&& other) noexcept -> FormData& {
            if (this != &other) {
                removeFiles();
                fields = std::move(other.fields);
                boundary = std::move(other.boundary);
                params = other.params;
                other.params.remove_uploaded_files = false;
            }
            return *this;
        }

        /**
         * @brief Remove owned upload files when params.remove_uploaded_files is set.
         */
        ~FormData() {
            removeFiles();
        }

        /**
         * @brief Find the first part with a name.
         * @param field_name Part name.
         * @return Copy of the part, or std::nullopt.
         */
        [[nodiscard]] auto FieldByName(std::string_view field_name) const -> std::optional<Field> {
            auto const it{ std::ranges::find(fields, field_name, &Field::name) };
            if (it == fields.end()) {
                return std::nullopt;
            }
            return *it;
        }

        /**
         * @brief Report whether a part with the name exists.
         * @param field_name Part name.
         * @return True when found.
         */
        [[nodiscard]] auto HasData(std::string_view field_name) const -> bool {
            return std::ranges::find(fields, field_name, &Field::name) != fields.end();
        }

        /**
         * @brief Report whether the named part carries content.
         * @param field_name Part name.
         * @return True when found and non-empty.
         */
        [[nodiscard]] auto HasContent(std::string_view field_name) const -> bool {
            auto const it{ std::ranges::find(fields, field_name, &Field::name) };
            return it != fields.end() && it->HasData();
        }

        /**
         * @brief Return the in-memory content of the named part.
         * @param field_name Part name.
         * @return Content, or std::nullopt when missing or empty.
         */
        [[nodiscard]] auto Content(
            std::string_view field_name
        ) const -> std::optional<std::string> {
            auto const it{ std::ranges::find(fields, field_name, &Field::name) };
            if (it == fields.end() || !it->HasData()) {
                return std::nullopt;
            }
            return it->content;
        }

        /**
         * @brief Describe every non-empty part for debugging.
         * @return Human-readable dump.
         */
        [[nodiscard]] auto Dump() const -> std::string {
            std::string out;
            for (auto const& field : fields) {
                if (!field.HasData()) {
                    continue;
                }
                out += std::format("{}:\n  type     = {}\n  filename = {}\n", field.name, field.content_type, field.filename);
                if (field.file_path) {
                    out += std::format("  file     = {}\n\n", field.file_path->string());
                } else {
                    out += std::format("  content  = {}\n\n", field.content);
                }
            }
            return out;
        }

    private:
        /**
         * @brief Remove owned files; a handler that moved a file away makes this a no-op.
         */
        auto removeFiles() noexcept -> void {
            if (!params.remove_uploaded_files) {
                return;
            }
            for (auto const& field : fields) {
                if (field.file_path) {
                    std::error_code ec;
                    std::filesystem::remove(*field.file_path, ec);
                }
            }
        }
    };

} // namespace mhttp
