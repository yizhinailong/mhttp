/**
 * @file headers.cppm
 * @brief Provide an HTTP header collection that either owns its fields or views a message's.
 */
module;

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/fields.hpp>

export module mhttp:headers;

import std;
import :config;

export namespace mhttp {

    /**
     * @brief Borrowed view of one header line.
     */
    struct HeaderView {
        Field name;                   ///< Known field, or Field::unknown for custom names.
        std::string_view name_string; ///< Field name as sent on the wire.
        std::string_view value;       ///< Field value.
    };

    /**
     * @brief HTTP header collection.
     *
     * Mirrors the owned/borrowed duality of std::string vs std::string_view: a default
     * constructed Headers owns its fields; Request::Base() and Response::Base() return borrowed
     * views whose writes land on the message. Copying a borrowed view yields the same view.
     */
    class Headers {
    public:
        /**
         * @brief Forward iterator over header lines in wire order; duplicates appear separately.
         */
        class ConstIterator {
        private:
            http::fields::const_iterator m_it; ///< Position in the underlying field list.

        public:
            using value_type = HeaderView;
            using reference = HeaderView;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;

            ConstIterator() = default;

            /**
             * @brief Wrap a field list position.
             * @param it Position to wrap.
             */
            explicit ConstIterator(http::fields::const_iterator it) : m_it{ it } {}

            auto operator*() const -> HeaderView {
                return HeaderView{ m_it->name(), detail::to_sv(m_it->name_string()), detail::to_sv(m_it->value()) };
            }

            auto operator++() -> ConstIterator& {
                ++m_it;
                return *this;
            }

            auto operator++(int) -> ConstIterator {
                auto copy{ *this };
                ++m_it;
                return copy;
            }

            auto operator==(ConstIterator const& other) const -> bool = default;
        };

    private:
        std::unique_ptr<http::fields> m_owned; ///< Owned storage; null for borrowed views.
        http::fields* m_fields{ nullptr };     ///< Active storage: m_owned.get() or the borrowed message.

    public:
        /**
         * @brief Create an empty, owning collection.
         */
        Headers() : m_owned{ std::make_unique<http::fields>() }, m_fields{ m_owned.get() } {}

        /**
         * @brief Copy: owned collections are deep-copied, borrowed views stay views.
         * @param other Collection to copy.
         */
        Headers(
            Headers const& other
        ) : m_owned{ other.m_owned ? std::make_unique<http::fields>(*other.m_owned) : nullptr }, m_fields{ m_owned ? m_owned.get() : other.m_fields } {}

        Headers(Headers&& other) noexcept = default;

        auto operator=(Headers const& other) -> Headers& {
            if (this != &other) {
                Headers copy{ other };
                *this = std::move(copy);
            }
            return *this;
        }

        auto operator=(Headers&& other) noexcept -> Headers& = default;

        ~Headers() = default;

        /**
         * @brief Create a view over an existing field list.
         * @param fields Field list that must outlive the view.
         * @return Borrowed collection.
         */
        static auto Borrow(http::fields& fields) noexcept -> Headers {
            Headers view{ BorrowTag{} };
            view.m_fields = &fields;
            return view;
        }

        /**
         * @brief Remove every field.
         */
        auto Clear() -> void {
            m_fields->clear();
        }

        /**
         * @brief Return the first value of a field.
         * @param name Field to look up.
         * @return Value, or an empty view when absent.
         */
        [[nodiscard]] auto operator[](Field name) const -> std::string_view {
            return detail::to_sv((*m_fields)[name]);
        }

        /**
         * @brief Return the first value of a field, matched case-insensitively.
         * @param name Field name.
         * @return Value, or an empty view when absent.
         */
        [[nodiscard]] auto operator[](std::string_view name) const -> std::string_view {
            return detail::to_sv((*m_fields)[name]);
        }

        /**
         * @brief Return the first value of a field.
         * @param name Field to look up.
         * @return Value.
         * @throws std::out_of_range If the field is absent.
         */
        [[nodiscard]] auto At(Field name) const -> std::string_view {
            return detail::to_sv(m_fields->at(name));
        }

        /**
         * @brief Return the first value of a field, matched case-insensitively.
         * @param name Field name.
         * @return Value.
         * @throws std::out_of_range If the field is absent.
         */
        [[nodiscard]] auto At(std::string_view name) const -> std::string_view {
            return detail::to_sv(m_fields->at(name));
        }

        /**
         * @brief Replace every value of a field with one value.
         * @param name Field to set.
         * @param value New value.
         */
        auto Set(Field name, std::string_view value) -> void {
            m_fields->set(name, value);
        }

        /**
         * @brief Replace every value of a field with one value.
         * @param name Field name.
         * @param value New value.
         */
        auto Set(std::string_view name, std::string_view value) -> void {
            m_fields->set(name, value);
        }

        /**
         * @brief Append a value, keeping existing values of the same field.
         * @param name Field to append.
         * @param value Value to append.
         */
        auto Insert(Field name, std::string_view value) -> void {
            m_fields->insert(name, value);
        }

        /**
         * @brief Append a value, keeping existing values of the same field.
         * @param name Field name.
         * @param value Value to append.
         */
        auto Insert(std::string_view name, std::string_view value) -> void {
            m_fields->insert(name, value);
        }

        /**
         * @brief Remove every value of a field.
         * @param name Field to remove.
         */
        auto Erase(Field name) -> void {
            m_fields->erase(name);
        }

        /**
         * @brief Remove every value of a field.
         * @param name Field name.
         */
        auto Erase(std::string_view name) -> void {
            m_fields->erase(name);
        }

        /**
         * @brief Report whether a field is present.
         * @param name Field to test.
         * @return True when at least one value exists.
         */
        [[nodiscard]] auto Has(Field name) const -> bool {
            return m_fields->find(name) != m_fields->end();
        }

        /**
         * @brief Report whether a field is present.
         * @param name Field name.
         * @return True when at least one value exists.
         */
        [[nodiscard]] auto Has(std::string_view name) const -> bool {
            return m_fields->find(name) != m_fields->end();
        }

        /**
         * @brief Count the values of a field.
         * @param name Field to count.
         * @return Number of lines; 1 means no duplicates.
         */
        [[nodiscard]] auto Count(Field name) const -> std::size_t {
            return m_fields->count(name);
        }

        /**
         * @brief Count the values of a field.
         * @param name Field name.
         * @return Number of lines; 1 means no duplicates.
         */
        [[nodiscard]] auto Count(std::string_view name) const -> std::size_t {
            return m_fields->count(name);
        }

        /**
         * @brief Return every value of a field in wire order.
         * @param name Field to collect.
         * @return Borrowed values.
         */
        [[nodiscard]] auto Values(Field name) const -> std::vector<std::string_view> {
            return collect(m_fields->equal_range(name));
        }

        /**
         * @brief Return every value of a field in wire order.
         * @param name Field name.
         * @return Borrowed values.
         */
        [[nodiscard]] auto Values(std::string_view name) const -> std::vector<std::string_view> {
            return collect(m_fields->equal_range(name));
        }

        [[nodiscard]] auto begin() const -> ConstIterator {
            return ConstIterator{ m_fields->begin() };
        }

        [[nodiscard]] auto end() const -> ConstIterator {
            return ConstIterator{ m_fields->end() };
        }

        /**
         * @brief Count every header line.
         * @return Number of lines, duplicates included.
         */
        [[nodiscard]] auto Size() const -> std::size_t {
            return static_cast<std::size_t>(std::distance(m_fields->begin(), m_fields->end()));
        }

        /**
         * @brief Report whether no header line exists.
         * @return True when empty.
         */
        [[nodiscard]] auto Empty() const -> bool {
            return m_fields->begin() == m_fields->end();
        }

        /**
         * @brief Append every line of another collection; same-name fields coexist.
         * @param other Lines to append.
         */
        auto Merge(Headers const& other) -> void {
            for (auto const& header : other) {
                m_fields->insert(header.name_string, header.value);
            }
        }

    private:
        struct BorrowTag {};

        /**
         * @brief Create an empty view; Borrow() points it at the message.
         */
        explicit Headers(BorrowTag) noexcept {}

        /**
         * @brief Collect the values in an equal_range result.
         * @param range Field range.
         * @return Borrowed values.
         */
        static auto collect(
            std::pair<http::fields::const_iterator, http::fields::const_iterator> range
        ) -> std::vector<std::string_view> {
            std::vector<std::string_view> out;
            for (auto it{ range.first }; it != range.second; ++it) {
                out.push_back(detail::to_sv(it->value()));
            }
            return out;
        }
    };

} // namespace mhttp
