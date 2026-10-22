/**
 * @file function.cppm
 * @brief Own move-only type-erased callables identically across standard library implementations.
 */
export module mhttp:core.function;

import std;

export namespace mhttp {

    /**
     * @brief Own a move-only callable matching a call signature.
     * @tparam Signature Unqualified function type such as R(Args...).
     */
    template <typename Signature>
    class MoveOnlyFunction;

    /**
     * @brief Own one move-only callable invoked through a non-const call operator.
     * @tparam R Invocation result; void discards the stored callable's result.
     * @tparam Args Parameters forwarded unchanged to the stored callable.
     * @note Covers the std::move_only_function subset used by mhttp, which not every standard
     *       library ships yet. Construction allocates once; moves transfer ownership and leave the
     *       source empty. Null function and member pointers construct an empty object.
     */
    template <typename R, typename... Args>
    class MoveOnlyFunction<R(Args...)> {
    private:
        /**
         * @brief Erase the stored callable behind one virtual invocation.
         */
        class Target {
        public:
            /**
             * @brief Destroy the stored callable and its owned captures.
             */
            virtual ~Target() = default;

            /**
             * @brief Invoke the stored callable.
             * @param args Arguments forwarded from the owning call operator.
             * @return Result converted to R.
             */
            virtual auto Invoke(Args&&... args) -> R = 0;
        };

        /**
         * @brief Store one decayed callable by value.
         * @tparam F Owned callable type.
         */
        template <typename F>
        class Holder final : public Target {
        private:
            F m_callable; ///< Owned callable and captures.

        public:
            /**
             * @brief Construct the stored callable from its source.
             * @tparam G Source type forwarded into F.
             * @param callable Source moved or copied into owned storage.
             */
            template <typename G>
            explicit Holder(G&& callable) : m_callable{ std::forward<G>(callable) } {}

            /**
             * @brief Invoke the stored callable as an lvalue.
             * @param args Arguments forwarded from the owning call operator.
             * @return Result converted to R.
             */
            auto Invoke(Args&&... args) -> R override {
                return std::invoke_r<R>(m_callable, std::forward<Args>(args)...);
            }
        };

        std::unique_ptr<Target> m_target; ///< Owned callable, or empty.

    public:
        /**
         * @brief Construct an empty function.
         */
        MoveOnlyFunction() noexcept = default;

        /**
         * @brief Construct an empty function from a null literal.
         */
        MoveOnlyFunction(std::nullptr_t) noexcept {}

        /**
         * @brief Take ownership of a callable invocable with this signature.
         * @tparam F Callable source; move-only captures are supported.
         * @param callable Source moved or copied into owned storage; null pointers leave this empty.
         * @throws std::bad_alloc If storage for the callable cannot be allocated.
         */
        template <typename F>
            requires (!std::same_as<std::remove_cvref_t<F>, MoveOnlyFunction>)
                && std::constructible_from<std::decay_t<F>, F>
                && std::is_invocable_r_v<R, std::decay_t<F>&, Args...>
        MoveOnlyFunction(F&& callable) {
            using Source = std::remove_cvref_t<F>;
            if constexpr (std::is_pointer_v<Source> || std::is_member_pointer_v<Source>) {
                if (callable == nullptr) { return; }
            }
            m_target = std::make_unique<Holder<std::decay_t<F>>>(std::forward<F>(callable));
        }

        /**
         * @brief Transfer ownership of the stored callable.
         * @param other Function left empty.
         */
        MoveOnlyFunction(MoveOnlyFunction&& other) noexcept = default;

        /**
         * @brief Prevent duplicating move-only captures.
         * @param other Unused source.
         */
        MoveOnlyFunction(MoveOnlyFunction const& other) = delete;

        /**
         * @brief Destroy the current callable and transfer ownership from another function.
         * @param other Function left empty.
         * @return This function.
         */
        auto operator=(MoveOnlyFunction&& other) noexcept -> MoveOnlyFunction& = default;

        /**
         * @brief Prevent duplicating move-only captures.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(MoveOnlyFunction const& other) -> MoveOnlyFunction& = delete;

        /**
         * @brief Destroy the stored callable, if any.
         */
        ~MoveOnlyFunction() = default;

        /**
         * @brief Observe whether a callable is stored.
         * @return False for empty or moved-from functions.
         */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_target); }

        /**
         * @brief Invoke the stored callable.
         * @param args Arguments forwarded to the callable.
         * @return Callable result converted to R.
         * @throws std::bad_function_call If this function is empty.
         */
        auto operator()(Args... args) -> R {
            if (!m_target) { throw std::bad_function_call{}; }
            return m_target->Invoke(std::forward<Args>(args)...);
        }
    };

} // namespace mhttp
