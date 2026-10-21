/**
 * @file router.cppm
 * @brief Register owned HTTP handlers and route aspects without exposing transport implementation types.
 */
export module mhttp:server.router;

import std;
import :server.impl.router;
export import :server.routing;
export import :server.static_files;

namespace mhttp::impl {

    class Session;
    class ServerImpl;

} // namespace mhttp::impl

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
        friend class impl::Session;
        friend class impl::ServerImpl;

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
         * @brief Mount regular files as a GET/HEAD fallback after ordinary route matching.
         * @param prefix Decoded absolute URL prefix, matched on segment boundaries; longest prefix wins.
         * @param root Native directory opened synchronously during configuration on a non-I/O thread.
         * @param options Owned index, range-count, and MIME override configuration.
         * @return Success, AlreadyRunning while frozen, Busy with readers/on I/O threads, or invalid configuration.
         * @note Duplicate prefixes replace prior mounts. Missing files run the normal not-found handler.
         *       All global hooks apply. Links/reparse points below root, dot segments, encoded separators,
         *       control bytes, DOS device names, alternate streams, and paths over 128 components are rejected.
         */
        [[nodiscard]] auto Mount(
            std::string_view prefix,
            std::filesystem::path const& root,
            StaticFileOptions options = {}
        ) -> Result<void> { return m_impl->Mount(prefix, root, std::move(options)); }

        /**
         * @brief Remove a static mount while configuration is mutable.
         * @param prefix Decoded absolute prefix, with the same canonicalization as Mount.
         * @return Whether a mount existed, or AlreadyRunning, Busy, or InvalidRequest.
         */
        [[nodiscard]] auto Unmount(std::string_view prefix) -> Result<bool> { return m_impl->Unmount(prefix); }

        /**
         * @brief Append global aspects around all routes and default 404/405 responses.
         * @tparam Aspects One or more objects providing Before and/or After returning bool or Task<bool>.
         * @param aspects Objects copied or moved into stable storage in registration order.
         * @return Success, AlreadyRunning while frozen, or Busy while requests are prepared.
         * @note Before false skips later Before and route execution, then runs every global After.
         *       Both phases run in registration order. After false skips the remaining global After.
         *       Exceptions skip later aspects and enter the router's exception response boundary.
         */
        template <typename... Aspects>
            requires (sizeof...(Aspects) > 0) && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto Use(Aspects&&... aspects) -> Result<void> {
            std::vector<detail::HttpMiddleware> incoming;
            incoming.reserve(sizeof...(Aspects));
            (incoming.push_back(detail::make_http_middleware(std::forward<Aspects>(aspects))), ...);
            return m_impl->Use(std::move(incoming));
        }

        /**
         * @brief Replace the custom 404 handler, with optional ordered route aspects.
         * @tparam F Synchronous void or Task<void> callback.
         * @tparam Aspects Eligible Before/After aspect types.
         * @param handler Callback copied or moved into stable storage.
         * @param aspects Aspects copied or moved in registration order.
         * @return Success, AlreadyRunning while frozen, or Busy while requests are prepared.
         * @note Called once inside global aspects if route execution produces 404; the default 404
         *       status is already set. Pre-routing interception skips this handler and route aspects.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto SetNotFoundHandler(F&& handler, Aspects&&... aspects) -> Result<void> {
            return m_impl->SetNotFoundHandler(
                detail::make_http_handler(std::forward<F>(handler), std::forward<Aspects>(aspects)...));
        }

        /**
         * @brief Replace the error response transformer, called once when status is at least 400.
         * @tparam F Synchronous void or Task<void> callback.
         * @param handler Owned callback accepting Request& and Response&.
         * @return Success, AlreadyRunning, or Busy.
         * @note Runs after global aspects and before post-routing; changing status does not recurse.
         */
        template <typename F>
            requires detail::HttpHandlerFunction<std::decay_t<F>>
        [[nodiscard]] auto SetErrorHandler(F&& handler) -> Result<void> {
            return m_impl->SetErrorHandler(detail::make_http_handler(std::forward<F>(handler)));
        }

        /**
         * @brief Replace exception response customization, called at most once per response before post-routing.
         * @tparam F Synchronous void or Task<void> callback.
         * @param handler Owned callback accepting Request&, Response&, and std::exception_ptr by value.
         * @return Success, AlreadyRunning, or Busy.
         * @note Partial output is replaced by a plain 500 before invocation. The callback may change
         *       the response, but the connection must still close. Its own failure restores a safe 500
         *       without recursion. Cancellation propagates; a post-routing failure closes without writing.
         */
        template <typename F>
            requires detail::HttpExceptionFunction<std::decay_t<F>>
        [[nodiscard]] auto SetExceptionHandler(F&& handler) -> Result<void> {
            return m_impl->SetExceptionHandler(detail::make_http_exception_handler(std::forward<F>(handler)));
        }

        /**
         * @brief Replace interception before route matching and body consumption.
         * @tparam F Callback returning bool or Task<bool>.
         * @param handler Owned callback accepting Request& and Response&.
         * @return Success, AlreadyRunning, or Busy.
         * @note True means handled: skip matching and all route/global aspects, preserve the response,
         *       then run error and post-routing hooks. False continues routing. Path captures and body
         *       are not populated yet; a session must close when interception leaves a body unread.
         */
        template <typename F>
            requires detail::HttpDecisionFunction<std::decay_t<F>>
        [[nodiscard]] auto SetPreRoutingHandler(F&& handler) -> Result<void> {
            return m_impl->SetPreRoutingHandler(detail::make_http_decision(std::forward<F>(handler)));
        }

        /**
         * @brief Replace the final response hook, invoked once after aspects and error transformation.
         * @tparam F Synchronous void or Task<void> callback.
         * @param handler Owned callback accepting Request& and Response&.
         * @return Success, AlreadyRunning, or Busy.
         * @note Applies to intercepted, missing, and matched requests. Its failure requires closing
         *       without writing and never invokes post-routing again. Serialization follows successful completion.
         */
        template <typename F>
            requires detail::HttpHandlerFunction<std::decay_t<F>>
        [[nodiscard]] auto SetPostRoutingHandler(F&& handler) -> Result<void> {
            return m_impl->SetPostRoutingHandler(detail::make_http_handler(std::forward<F>(handler)));
        }

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
         * @brief Register a handler that may pull bounded request chunks before the full upload arrives.
         * @tparam F Synchronous void or Task<void> callable accepting Request& and Response&.
         * @tparam Aspects Owned ordered Before/After objects; global middleware still wraps the route.
         * @param method Supported method; all normal pattern matching and HEAD fallback rules apply.
         * @param path Decoded route pattern, copied during registration.
         * @param handler Callable copied or moved into stable storage; await Request::ReadBody to read.
         * @param aspects Aspects copied or moved in registration order.
         * @return Registration result as for Route; replacing a binding also replaces its intake policy.
         * @note Unread body bytes force close after the response. Body() stays empty; trailers appear
         *       after ReadBody returns nullopt. Standalone dispatch has no network body to read.
         */
        template <typename F, typename... Aspects>
            requires detail::HttpHandlerFunction<std::decay_t<F>> && (detail::HttpAspect<std::decay_t<Aspects>> && ...)
        [[nodiscard]] auto RouteLazy(
            mhttp::Method method,
            std::string_view path,
            F&& handler,
            Aspects&&... aspects
        ) -> Result<void> {
            return m_impl->Register(method, path,
                detail::make_http_handler(std::forward<F>(handler), std::forward<Aspects>(aspects)...), true);
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
         * @brief Retain configuration for an already initialized protocol error, bypassing pre-routing.
         * @param router Borrowed configuration wrapper only during this call.
         * @return Owned response-write lease; Process still invokes error and post-routing hooks.
         * @note The caller initializes a sanitized Request and error Response and must close the connection.
         */
        static auto PrepareError(server::Router& router) -> PreparedRoute {
            return router.m_impl->PrepareError();
        }

        /**
         * @brief Execute a prepared handler or default 404/405 response without borrowing Router.
         * @param route Owned prepared state, moved into this operation.
         * @param request Borrowed through completion.
         * @param response Borrowed through completion.
         * @return Lazy execution Task with finalized response/connection policy; cancellation propagates.
         */
        static auto Process(
            PreparedRoute route,
            server::Request& request,
            server::Response& response
        ) -> Task<RoutingOutcome> { return impl::RouterImpl::Process(std::move(route), request, response); }

        /**
         * @brief Prepare and execute in one operation, primarily for in-process tests and adapters.
         * @param router Borrowed only during operation creation.
         * @param request Borrowed through completion.
         * @param response Borrowed through completion.
         * @return Task already retaining configuration ownership before initial suspension, with response policy.
         */
        static auto Dispatch(
            server::Router& router,
            server::Request& request,
            server::Response& response
        ) -> Task<RoutingOutcome> {
            return impl::RouterImpl::Dispatch(router.m_impl->Prepare(request, response), request, response);
        }
    };

} // namespace mhttp::detail
