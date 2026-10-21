/**
 * @file request_data.cppm
 * @brief Share typed, tagged request context safely between middleware and handlers.
 */
export module mhttp:server.request_data;

import std;

export namespace mhttp::server {

    /**
     * @brief Own copyable context values keyed by their decayed type and an optional case-sensitive tag.
     * @note Individual operations are thread-safe. Fetch copies an immutable snapshot, so it neither
     *       consumes storage nor depends on a separate Has call. User copying and destruction run
     *       outside the map lock. Destruction of this container requires all callers to have finished.
     */
    class RequestData {
    private:
        /**
         * @brief Keep runtime type identity separate from arbitrary binary tag bytes.
         */
        struct Key {
            std::type_index type; ///< Decayed stored type, independent of type-name spelling.
            std::string tag; ///< Owned tag bytes, including embedded NUL.

            /**
             * @brief Compare exact type and tag identity.
             * @param other Lookup key.
             * @return Whether both keys identify the same slot.
             */
            auto operator==(Key const& other) const -> bool = default;
        };

        /**
         * @brief Hash type identity and tag bytes together.
         */
        struct KeyHash {
            /**
             * @brief Hash a borrowed lookup key.
             * @param key Exact slot identity.
             * @return Combined hash; equality still checks both components.
             */
            auto operator()(Key const& key) const noexcept -> std::size_t {
                auto const type_hash = key.type.hash_code();
                auto const tag_hash = std::hash<std::string>{}(key.tag);
                return type_hash ^ (tag_hash + 0x9e3779b9U + (type_hash << 6) + (type_hash >> 2));
            }
        };

        mutable std::mutex m_mutex; ///< Protect only key lookup and immutable snapshot publication.
        std::unordered_map<Key, std::shared_ptr<std::any const>, KeyHash> m_values; ///< Owned context snapshots.

    public:
        /**
         * @brief Store a copied or moved value without a tag, replacing the same type's previous value.
         * @tparam T Copyable decayed stored type; references and cv qualifiers are removed.
         * @param value Value copied or moved into independently owned storage.
         */
        template <typename T>
            requires std::is_copy_constructible_v<std::decay_t<T>>
                && std::constructible_from<std::decay_t<T>, T>
        auto Store(T&& value) -> void { Store({}, std::forward<T>(value)); }

        /**
         * @brief Store a typed value under an owned tag, replacing only the same type and tag.
         * @tparam T Copyable decayed stored type.
         * @param tag Borrowed case-sensitive tag, copied for storage.
         * @param value Value copied or moved before acquiring the map lock.
         */
        template <typename T>
            requires std::is_copy_constructible_v<std::decay_t<T>>
                && std::constructible_from<std::decay_t<T>, T>
        auto Store(std::string_view tag, T&& value) -> void {
            auto key = Key{ typeid(std::decay_t<T>), std::string{ tag } };
            auto snapshot = std::make_shared<std::any const>(std::in_place_type<std::decay_t<T>>, std::forward<T>(value));
            {
                std::lock_guard lock{ m_mutex };
                auto entry = m_values.try_emplace(std::move(key)).first;
                entry->second.swap(snapshot);
            }
        }

        /**
         * @brief Copy one consistent snapshot without consuming its stored value.
         * @tparam T Requested type; cv qualifiers and references are removed for lookup and return.
         * @param tag Borrowed case-sensitive tag, empty by default.
         * @return Independent value copy, or nullopt when the slot is absent.
         * @note Exceptions from the stored type's copy constructor propagate after releasing the lock.
         */
        template <typename T>
            requires std::is_copy_constructible_v<std::decay_t<T>>
        [[nodiscard]] auto Fetch(std::string_view tag = {}) const -> std::optional<std::decay_t<T>> {
            auto key = Key{ typeid(std::decay_t<T>), std::string{ tag } };
            std::shared_ptr<std::any const> snapshot;
            {
                std::lock_guard lock{ m_mutex };
                auto const entry = m_values.find(key);
                if (entry == m_values.end()) { return std::nullopt; }
                snapshot = entry->second;
            }
            return std::any_cast<std::decay_t<T>>(*snapshot);
        }

        /**
         * @brief Test exact slot presence without copying a stored value.
         * @tparam T Requested decayed type.
         * @param tag Borrowed case-sensitive tag, empty by default.
         * @return Whether the slot exists at this call's lookup instant.
         * @note Use Fetch directly when a value is needed; a subsequent lookup may see a concurrent erase.
         */
        template <typename T>
        [[nodiscard]] auto Has(std::string_view tag = {}) const -> bool {
            auto key = Key{ typeid(std::decay_t<T>), std::string{ tag } };
            std::lock_guard lock{ m_mutex };
            return m_values.contains(key);
        }

        /**
         * @brief Remove an exact slot, destroying its value after releasing the map lock.
         * @tparam T Requested decayed type.
         * @param tag Borrowed case-sensitive tag, empty by default.
         * @return Whether a slot was removed; already fetched copies and snapshots remain valid.
         */
        template <typename T>
        auto Erase(std::string_view tag = {}) -> bool {
            auto key = Key{ typeid(std::decay_t<T>), std::string{ tag } };
            std::shared_ptr<std::any const> removed;
            {
                std::lock_guard lock{ m_mutex };
                auto const entry = m_values.find(key);
                if (entry == m_values.end()) { return false; }
                removed = std::move(entry->second);
                m_values.erase(entry);
            }
            return true;
        }
    };

} // namespace mhttp::server
