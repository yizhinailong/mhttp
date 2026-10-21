/**
 * @file router.cppm
 * @brief Register owned HTTP handlers and route aspects without exposing transport implementation types.
 */
export module mhttp:server.router;

import std;
import :server.impl.router;
export import :server.routing;

export namespace mhttp::detail {

    struct RouterAccess;

} // namespace mhttp::detail

export namespace mhttp::server {

    /**
     * @brief Configure route regular-expression source and decoded subject bounds at construction.
     */
    using RouterOptions = detail::RouteLimits;

    /**
     * @brief Own route configuration with stable identity for an asynchronous server.
     * @note Register before startup. Exact pattern/method duplicates replace the previous callback
     *       without changing priority. Frozen configuration returns AlreadyRunning; pending standalone
     *       routing returns Busy. Registration and request preparation are synchronized. Handlers may
     *       run concurrently, so application callback state requires its own synchronization.
     */
    class Router {
    private:
        std::shared_ptr<impl::RouterImpl> m_impl; ///< Owned tree, handlers, and configuration guards.
        friend struct detail::RouterAccess;

    public:
        /**
         * @brief Create empty routes with fixed expression bounds.
         * @param options Maximum regex source and decoded subject lengths.
         */
        explicit Router(RouterOptions options = {}) : m_impl{ std::make_shared<impl::RouterImpl>(options) } {}

        /**
         * @brief Keep configuration identity unique while prepared operations retain its state.
         * @param other Unused source.
         */
        Router(Router const& other) = delete;

        /**
         * @brief Prevent copied configuration aliases and implicit moves.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(Router const& other) -> Router& = delete;

        /**
         * @brief Release this wrapper while issued Tasks retain their own callback and configuration ownership.
         */
        virtual ~Router() = default;

        /**
         * @brief Register or replace one method-specific handler with optional ordered aspects.
         * @tparam F Synchronous void or Task<void> callable accepting Request& and Response&.
         * @tparam Aspects Owned Before/After objects returning bool or Task<bool>.
         * @param method Supported method.
         * @param path Decoded route pattern, copied during registration.
         * @param handler Callable copied or moved into stable storage.
         * @param aspects Aspects copied or moved in registration order.
         * @return Success, InvalidRequest, AlreadyRunning while frozen, or Busy while requests are prepared.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Route(
            mhttp::Method method,
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> {
            return m_impl->Register(method, path,
                detail::make_http_handler(std::forward<F>(handler), std::forward<Aspects>(aspects)...));
        }

        /**
         * @brief Register a GET handler, also eligible for implicit HEAD fallback.
         * @tparam F Eligible handler type.
         * @tparam Aspects Eligible ordered aspect types.
         * @param path Decoded route pattern.
         * @param handler Owned callback.
         * @param aspects Owned route aspects.
         * @return Registration result as for Route.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Get(
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> { return Route(mhttp::Method::Get, path, std::forward<F>(handler), std::forward<Aspects>(aspects)...); }

        /**
         * @brief Register an explicit HEAD handler before considering GET fallback.
         * @tparam F Eligible handler type.
         * @tparam Aspects Eligible ordered aspect types.
         * @param path Decoded route pattern.
         * @param handler Owned callback.
         * @param aspects Owned route aspects.
         * @return Registration result as for Route.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Head(
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> { return Route(mhttp::Method::Head, path, std::forward<F>(handler), std::forward<Aspects>(aspects)...); }

        /**
         * @brief Register a POST handler.
         * @tparam F Eligible handler type.
         * @tparam Aspects Eligible ordered aspect types.
         * @param path Decoded route pattern.
         * @param handler Owned callback.
         * @param aspects Owned route aspects.
         * @return Registration result as for Route.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Post(
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> { return Route(mhttp::Method::Post, path, std::forward<F>(handler), std::forward<Aspects>(aspects)...); }

        /**
         * @brief Register a PUT handler.
         * @tparam F Eligible handler type.
         * @tparam Aspects Eligible ordered aspect types.
         * @param path Decoded route pattern.
         * @param handler Owned callback.
         * @param aspects Owned route aspects.
         * @return Registration result as for Route.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Put(
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> { return Route(mhttp::Method::Put, path, std::forward<F>(handler), std::forward<Aspects>(aspects)...); }

        /**
         * @brief Register a PATCH handler.
         * @tparam F Eligible handler type.
         * @tparam Aspects Eligible ordered aspect types.
         * @param path Decoded route pattern.
         * @param handler Owned callback.
         * @param aspects Owned route aspects.
         * @return Registration result as for Route.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Patch(
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> { return Route(mhttp::Method::Patch, path, std::forward<F>(handler), std::forward<Aspects>(aspects)...); }

        /**
         * @brief Register a DELETE handler without platform macro conflicts.
         * @tparam F Eligible handler type.
         * @tparam Aspects Eligible ordered aspect types.
         * @param path Decoded route pattern.
         * @param handler Owned callback.
         * @param aspects Owned route aspects.
         * @return Registration result as for Route.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Delete(
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> { return Route(mhttp::Method::Delete, path, std::forward<F>(handler), std::forward<Aspects>(aspects)...); }

        /**
         * @brief Register an OPTIONS handler, including for the literal asterisk target.
         * @tparam F Eligible handler type.
         * @tparam Aspects Eligible ordered aspect types.
         * @param path Decoded route pattern, or *.
         * @param handler Owned callback.
         * @param aspects Owned route aspects.
         * @return Registration result as for Route.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Options(
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> { return Route(mhttp::Method::Options, path, std::forward<F>(handler), std::forward<Aspects>(aspects)...); }
    };

} // namespace mhttp::server

export namespace mhttp::detail {

    /**
     * @brief Integrate route preparation, execution, and configuration freezing with a session lifecycle.
     * @internal
     */
    struct RouterAccess {
        /**
         * @brief Freeze configuration before accepting connections.
         * @param router Borrowed configuration owner.
         * @return Success, AlreadyRunning, or Busy.
         */
        static auto Freeze(server::Router& router) -> Result<void> { return router.m_impl->Freeze(); }

        /**
         * @brief Thaw configuration after every session and prepared operation releases its lease.
         * @param router Borrowed configuration owner.
         * @return Success or Busy; idle thaw is idempotent.
         */
        static auto Thaw(server::Router& router) -> Result<void> { return router.m_impl->Thaw(); }

        /**
         * @brief Retain state before initial suspension and populate captures before body/handler execution.
         * @param router Borrowed only for issuing this operation; the Task owns its state independently.
         * @param request Borrowed through the session's entire handler execution.
         * @param response Borrowed for pre-routing interception and execution.
         * @return Lazy preparation Task; its result keeps configuration guarded through response writes.
         */
        static auto Prepare(
            server::Router& router,
            server::Request& request,
            server::Response& response
        ) -> Task<PreparedRoute> { return router.m_impl->Prepare(request, response); }

        /**
         * @brief Execute a prepared handler or default 404/405 response without borrowing Router.
         * @param route Owned prepared state, moved into this operation.
         * @param request Borrowed through completion.
         * @param response Borrowed through completion.
         * @return Lazy execution Task, propagating callback exceptions to the session.
         */
        static auto Process(
            PreparedRoute route,
            server::Request& request,
            server::Response& response
        ) -> Task<void> { return impl::RouterImpl::Process(std::move(route), request, response); }

        /**
         * @brief Prepare and execute in one operation, primarily for in-process tests and adapters.
         * @param router Borrowed only during operation creation.
         * @param request Borrowed through completion.
         * @param response Borrowed through completion.
         * @return Task already retaining configuration ownership before initial suspension.
         */
        static auto Dispatch(
            server::Router& router,
            server::Request& request,
            server::Response& response
        ) -> Task<void> {
            return impl::RouterImpl::Dispatch(router.m_impl->Prepare(request, response), request, response);
        }
    };

} // namespace mhttp::detail
