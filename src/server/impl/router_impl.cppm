/**
 * @file router_impl.cppm
 * @brief Own guarded route configuration and prepared callback lifetimes independently of server I/O.
 */
module mhttp:server.impl.router;

import std;
import :server.routing;
import :server.impl.static_response;
import :server.impl.static_policy;
import :server.static_files;

namespace mhttp::impl {

    /**
     * @brief Serialize configuration changes while allowing immutable matching and callbacks to run concurrently.
     */
    class RouterImpl : public std::enable_shared_from_this<RouterImpl> {
    private:
        /**
         * @brief Retain router ownership and one configuration reader through lazy Tasks and response writes.
         */
        struct Lease {
            std::shared_ptr<RouterImpl> owner; ///< Router kept alive until reader release.
            bool active{}; ///< Reader registered under its owner's mutex before publication.

            /**
             * @brief Own state before registering the reader, preserving allocation failure safety.
             * @param router Owned router state.
             */
            explicit Lease(std::shared_ptr<RouterImpl> router) : owner{ std::move(router) } {}

            /**
             * @brief Release the reader after all prepared route copies and Tasks finish.
             */
            ~Lease() {
                if (active) {
                    std::lock_guard lock{ owner->m_mutex };
                    --owner->m_readers;
                }
            }
        };

        /**
         * @brief Keep body admission policy inseparable from its stable owned callback.
         */
        struct RouteHandler {
            detail::HttpHandler handler; ///< Owned adapted route and ordered aspects.
            bool lazy{}; ///< Dispatch after the head without buffering the body.
        };

        std::mutex m_mutex; ///< Protect configuration, freeze state, and reader registration.
        detail::RouteTree m_tree; ///< Immutable while frozen or any configuration reader exists.
        std::unordered_map<std::size_t, std::shared_ptr<RouteHandler const>> m_handlers; ///< Owned bindings.
        std::vector<std::shared_ptr<StaticMount const>> m_mounts; ///< Longest-prefix static fallbacks.
        std::vector<detail::HttpMiddleware> m_middleware; ///< Global aspects, immutable while readers exist.
        std::shared_ptr<detail::HttpHandler const> m_not_found; ///< Optional 404 handler with its own aspects.
        std::shared_ptr<detail::HttpHandler const> m_error_handler; ///< Optional final error response transformation.
        std::shared_ptr<detail::HttpExceptionHandler const> m_exception_handler; ///< Optional exception response customization.
        std::shared_ptr<detail::HttpDecision const> m_pre_routing; ///< Optional interception before path selection.
        std::shared_ptr<detail::HttpHandler const> m_post_routing; ///< Optional final response mutation hook.
        std::size_t m_next_id{}; ///< Next opaque binding identifier.
        std::size_t m_readers{}; ///< Prepared operations, including Tasks not yet started.
        bool m_frozen{}; ///< Server lifecycle rejects mutation until all sessions drain and thaw succeeds.

    public:
        /**
         * @brief Validate and open a static fallback root outside the configuration mutex.
         * @param prefix Decoded portable absolute prefix.
         * @param root Native directory to pin until replacement, removal, or router destruction.
         * @param options Owned static response configuration.
         * @return Success, AlreadyRunning, Busy, or a configuration error without replacing a prior mount.
         */
        auto Mount(
            std::string_view prefix,
            std::filesystem::path const& root,
            server::StaticFileOptions options
        ) -> Result<void> {
            {
                std::lock_guard lock{ m_mutex };
                if (m_frozen) { return std::unexpected{ Error::AlreadyRunning }; }
                if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            }
            auto incoming = StaticMount::Create(prefix, root, std::move(options));
            if (!incoming) { return std::unexpected{ incoming.error() }; }
            std::shared_ptr<StaticMount const> retired;
            std::lock_guard lock{ m_mutex };
            if (m_frozen) { return std::unexpected{ Error::AlreadyRunning }; }
            if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            for (auto& mount : m_mounts) {
                if (mount->data->key == (*incoming)->data->key) { retired = std::exchange(mount, std::move(*incoming)); return {}; }
            }
            m_mounts.push_back(std::move(*incoming));
            return {};
        }

        /**
         * @brief Remove a configured static fallback without releasing native resources under the mutex.
         * @param prefix Decoded absolute prefix, canonicalized as during registration.
         * @return Whether a mount existed, or AlreadyRunning, Busy, or InvalidRequest.
         */
        auto Unmount(std::string_view prefix) -> Result<bool> {
            auto parts = static_components(prefix, false);
            if (!parts) { return std::unexpected{ Error::InvalidRequest }; }
            std::shared_ptr<StaticMount const> retired;
            std::lock_guard lock{ m_mutex };
            if (m_frozen) { return std::unexpected{ Error::AlreadyRunning }; }
            if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            for (auto it = m_mounts.begin(); it != m_mounts.end(); ++it) {
                if ((*it)->data->prefix == *parts) { retired = std::move(*it); m_mounts.erase(it); return true; }
            }
            return false;
        }

        /**
         * @brief Create empty route configuration with fixed regular-expression bounds.
         * @param limits Pattern source and decoded matching subject limits.
         */
        explicit RouterImpl(detail::RouteLimits limits) : m_tree{ limits } {}

        /**
         * @brief Atomically append global aspects after constructing all user objects outside the mutex.
         * @param incoming Owned middleware in registration order.
         * @return Success, AlreadyRunning while frozen, or Busy while readers exist.
         */
        auto Use(std::vector<detail::HttpMiddleware> incoming) -> Result<void> {
            std::lock_guard lock{ m_mutex };
            if (m_frozen) { return std::unexpected{ Error::AlreadyRunning }; }
            if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            m_middleware.reserve(m_middleware.size() + incoming.size());
            for (auto& middleware : incoming) { m_middleware.push_back(std::move(middleware)); }
            return {};
        }

        /**
         * @brief Replace the custom handler for a route result with status 404.
         * @param handler Owned callback and optional aspects.
         * @return Success, AlreadyRunning, or Busy.
         */
        auto SetNotFoundHandler(detail::HttpHandler handler) -> Result<void> {
            return replaceCallback(m_not_found, std::move(handler));
        }

        /**
         * @brief Replace the response transformer run once when status is at least 400.
         * @param handler Owned callback.
         * @return Success, AlreadyRunning, or Busy.
         */
        auto SetErrorHandler(detail::HttpHandler handler) -> Result<void> {
            return replaceCallback(m_error_handler, std::move(handler));
        }

        /**
         * @brief Replace the handler for contained callback exceptions.
         * @param handler Owned callback receiving the original exception pointer.
         * @return Success, AlreadyRunning, or Busy.
         */
        auto SetExceptionHandler(detail::HttpExceptionHandler handler) -> Result<void> {
            return replaceCallback(m_exception_handler, std::move(handler));
        }

        /**
         * @brief Replace interception before matching and body consumption.
         * @param handler Owned callback returning true when it handled the request.
         * @return Success, AlreadyRunning, or Busy.
         */
        auto SetPreRoutingHandler(detail::HttpDecision handler) -> Result<void> {
            return replaceCallback(m_pre_routing, std::move(handler));
        }

        /**
         * @brief Replace the final response hook, run once after error response transformation.
         * @param handler Owned callback.
         * @return Success, AlreadyRunning, or Busy.
         */
        auto SetPostRoutingHandler(detail::HttpHandler handler) -> Result<void> {
            return replaceCallback(m_post_routing, std::move(handler));
        }

        /**
         * @brief Register or replace a callback without running user destruction code under the mutex.
         * @param method Supported method.
         * @param path Borrowed decoded registration pattern.
         * @param handler Owned callable adapter.
         * @param lazy Whether the route pulls its body after head admission.
         * @return Success, AlreadyRunning while frozen, Busy while prepared readers exist, or InvalidRequest.
         */
        auto Register(
            Method method,
            std::string_view path,
            detail::HttpHandler handler,
            bool lazy = false
        ) -> Result<void> {
            return Register(std::span<Method const>{ &method, 1 }, path, std::move(handler), lazy);
        }

        /**
         * @brief Publish method bindings under one configuration guard, sharing owned callback state.
         * @param methods Nonempty borrowed method list, validated before any binding is changed.
         * @param path Borrowed decoded pattern, validated before its first binding is inserted.
         * @param handler Owned callable adapter shared by every listed method.
         * @param lazy Whether the routes pull their bodies after head admission.
         * @return Success, AlreadyRunning, Busy, or InvalidRequest without changing existing bindings.
         * @note Replaced callback state is destroyed after releasing the configuration mutex.
         */
        auto Register(
            std::span<Method const> methods,
            std::string_view path,
            detail::HttpHandler handler,
            bool lazy = false
        ) -> Result<void> {
            auto incoming = std::make_shared<RouteHandler const>(RouteHandler{ std::move(handler), lazy });
            std::vector<std::shared_ptr<RouteHandler const>> retired;
            retired.reserve(methods.size());
            std::lock_guard lock{ m_mutex };
            if (m_frozen) { return std::unexpected{ Error::AlreadyRunning }; }
            if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            for (auto selected_method : methods) {
                if (ToString(selected_method).empty()) { return std::unexpected{ Error::InvalidRequest }; }
            }
            for (auto selected_method : methods) {
                auto const id = m_next_id;
                m_handlers.emplace(id, incoming);
                try {
                    auto binding = m_tree.Insert(selected_method, path, id);
                    if (!binding) {
                        m_handlers.erase(id);
                        return std::unexpected{ binding.error() };
                    }
                    if (binding->has_value()) {
                        auto previous = m_handlers.find(**binding);
                        retired.push_back(std::move(previous->second));
                        m_handlers.erase(previous);
                    }
                    ++m_next_id;
                } catch (...) {
                    m_handlers.erase(id);
                    throw;
                }
            }
            return {};
        }

        /**
         * @brief Freeze configuration before a server starts accepting requests.
         * @return Success, AlreadyRunning if already frozen, or Busy if standalone requests are prepared.
         */
        auto Freeze() -> Result<void> {
            std::lock_guard lock{ m_mutex };
            if (m_frozen) { return std::unexpected{ Error::AlreadyRunning }; }
            if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            m_frozen = true;
            return {};
        }

        /**
         * @brief Permit configuration changes only after every prepared operation and session has drained.
         * @return Success, or Busy while readers still hold configuration leases.
         */
        auto Thaw() -> Result<void> {
            std::lock_guard lock{ m_mutex };
            if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            m_frozen = false;
            return {};
        }

        /**
         * @brief Retain configuration ownership before returning a lazy preparation Task.
         * @param request Borrowed initialized request, alive through preparation and execution.
         * @param response Borrowed response for pre-routing interception.
         * @return Lazy preparation Task; it already prevents configuration mutation before initial suspension.
         */
        auto Prepare(
            server::Request& request,
            server::Response& response
        ) -> Task<detail::PreparedRoute> {
            return prepare(acquireLease(), request, response);
        }

        /**
         * @brief Retain configuration for a prebuilt protocol error without matching or invoking pre-routing.
         * @return Prepared state; the caller supplies the error response and enforces connection closure.
         */
        auto PrepareError() -> detail::PreparedRoute {
            return { .lifetime = acquireLease(), .handled = true };
        }

        /**
         * @brief Contain callback exceptions, finalize the response, and return an explicit connection policy.
         * @param route Owned callback and configuration lease, retained in the coroutine frame.
         * @param request Borrowed until the complete handler/aspect chain finishes.
         * @param response Borrowed for response mutation through the same lifetime.
         * @return Final connection policy; cancellation still propagates to the session boundary.
         * @note Keep another prepared lease through writes when execution completes before the session
         *       releases its final route reference. Allocation failures may still propagate.
         */
        static auto Process(
            detail::PreparedRoute route,
            server::Request& request,
            server::Response& response
        ) -> Task<detail::RoutingOutcome> {
            auto& owner = *std::static_pointer_cast<Lease>(route.lifetime)->owner;
            detail::RoutingOutcome result{ .failure = route.failure };
            if (!result.failure && !route.handled) {
                try { co_await owner.processRoute(route, request, response); }
                catch (...) { result.failure = std::current_exception(); }
            }
            detail::ServerRequestAccess::RetireBody(request);
            if (result.failure) {
                result.close = true;
                result.failure = co_await owner.processException(result.failure, request, response, true);
            }
            if (auto body_failure = detail::ServerRequestAccess::BodyFailure(request)) {
                if (body_failure->error == Error::Canceled) { throw std::system_error{ make_error_code(Error::Canceled) }; }
                result.close = true;
                response = server::Response{};
                static_cast<void>(response.SetError(body_failure->status));
            }
            std::exception_ptr error_failure;
            if (static_cast<unsigned>(response.Status()) >= 400 && owner.m_error_handler) {
                try { co_await (*owner.m_error_handler)(request, response); }
                catch (...) { error_failure = std::current_exception(); }
            }
            if (error_failure) {
                auto const invoke_handler = !result.close;
                result.close = true;
                result.failure = co_await owner.processException(error_failure, request, response, invoke_handler);
            }
            if (owner.m_post_routing) {
                try { co_await (*owner.m_post_routing)(request, response); }
                catch (...) { result.failure = std::current_exception(); result.close = true; result.send_response = false; }
                rethrowCancellation(result.failure);
            }
            co_return result;
        }

        /**
         * @brief Consume preparation and execution as one lazy operation for in-process routing.
         * @param pending Owned preparation Task that already holds its configuration lease.
         * @param request Borrowed through completion.
         * @param response Borrowed through completion.
         * @return Finalized response and connection policy.
         */
        static auto Dispatch(
            Task<detail::PreparedRoute> pending,
            server::Request& request,
            server::Response& response
        ) -> Task<detail::RoutingOutcome> {
            auto route = co_await std::move(pending);
            co_return co_await Process(std::move(route), request, response);
        }

    private:
        /**
         * @brief Acquire owned reader state synchronously before publishing a lazy Task or prepared response.
         * @return Registered reader that releases the configuration guard with its final owner.
         */
        auto acquireLease() -> std::shared_ptr<Lease> {
            auto lease = std::make_shared<Lease>(shared_from_this());
            {
                std::lock_guard lock{ m_mutex };
                ++m_readers;
                lease->active = true;
            }
            return lease;
        }

        /**
         * @brief Preserve cancellation as control flow instead of converting runtime shutdown into HTTP 500.
         * @param failure Optional callback exception.
         * @throws std::system_error If the original failure has Error::Canceled.
         */
        static auto rethrowCancellation(std::exception_ptr const& failure) -> void {
            if (!failure) { return; }
            try { std::rethrow_exception(failure); }
            catch (std::system_error const& error) { if (error.code() == Error::Canceled) { throw; } }
            catch (...) {}
        }

        /**
         * @brief Execute the global/route layers without catching or resuming after callback failures.
         * @param route Prepared route borrowed from the owning Process frame.
         * @param request Borrowed until all aspects and handlers finish.
         * @param response Borrowed mutable response.
         */
        auto processRoute(
            detail::PreparedRoute const& route,
            server::Request& request,
            server::Response& response
        ) -> Task<void> {
            bool proceed{ true };
            for (auto const& middleware : m_middleware) {
                if (!co_await middleware.before(request, response)) { proceed = false; break; }
            }
            if (proceed) {
                if (route.handler) { co_await (*route.handler)(request, response); }
                else {
                    static_cast<void>(response.SetError(route.match.status));
                    if (!route.match.allow.empty()) {
                        static_cast<void>(response.Headers().Set("Allow", route.match.allow));
                    }
                }
                if (response.Status() == Status::NotFound && m_not_found) {
                    co_await (*m_not_found)(request, response);
                }
            }
            for (auto const& middleware : m_middleware) {
                if (!co_await middleware.after(request, response)) { break; }
            }
        }

        /**
         * @brief Replace partial output with a safe 500 and contain a failing exception callback without recursion.
         * @param failure Owned original exception.
         * @param request Borrowed request.
         * @param response Borrowed response, reset before customization.
         * @param invoke_handler Whether this is the first exception boundary for this response.
         * @return Original failure, or the exception callback's failure if customization failed.
         * @note The outer Process frame owns this router through the prepared lease.
         */
        auto processException(
            std::exception_ptr failure,
            server::Request& request,
            server::Response& response,
            bool invoke_handler
        ) -> Task<std::exception_ptr> {
            rethrowCancellation(failure);
            response = server::Response{};
            static_cast<void>(response.SetError(Status::InternalServerError));
            if (invoke_handler && m_exception_handler) {
                std::exception_ptr secondary;
                try { co_await (*m_exception_handler)(request, response, failure); }
                catch (...) { secondary = std::current_exception(); }
                if (secondary) {
                    rethrowCancellation(secondary);
                    response = server::Response{};
                    static_cast<void>(response.SetError(Status::InternalServerError));
                    failure = std::move(secondary);
                }
            }
            co_return failure;
        }

        /**
         * @brief Replace callback ownership while retiring user resources after releasing the mutex.
         * @tparam F Type-erased callback type.
         * @param slot Configuration member protected by this router's lifecycle guard.
         * @param handler Owned incoming callback, constructed before acquiring the mutex.
         * @return Success, AlreadyRunning, or Busy.
         */
        template <typename F>
        auto replaceCallback(std::shared_ptr<F const>& slot, F handler) -> Result<void> {
            auto incoming = std::make_shared<F const>(std::move(handler));
            std::shared_ptr<F const> retired;
            std::lock_guard lock{ m_mutex };
            if (m_frozen) { return std::unexpected{ Error::AlreadyRunning }; }
            if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            retired = std::exchange(slot, std::move(incoming));
            return {};
        }

        /**
         * @brief Match immutable state after reader acquisition, then publish request-owned captures.
         * @param lease Owned registered configuration reader.
         * @param request Borrowed initialized request.
         * @param response Borrowed response for pre-routing interception before body intake.
         * @return Prepared callback and lease, independent of the public Router object's lifetime.
         */
        static auto prepare(
            std::shared_ptr<Lease> lease,
            server::Request& request,
            server::Response& response
        ) -> Task<detail::PreparedRoute> {
            auto& owner = *lease->owner;
            detail::PreparedRoute route{ .lifetime = std::move(lease) };
            try {
                detail::ServerRequestAccess::SetPathParams(request, {});
                if (owner.m_pre_routing && co_await (*owner.m_pre_routing)(request, response)) {
                    route.handled = true;
                } else {
                    route.match = owner.m_tree.Match(request.MethodString(), detail::ServerRequestAccess::EncodedPath(request));
                    if (route.match.handler) {
                        auto selected = std::as_const(owner.m_handlers).at(*route.match.handler);
                        route.handler = std::shared_ptr<detail::HttpHandler const>{ selected, &selected->handler };
                        route.lazy = selected->lazy;
                    } else if (route.match.status == Status::NotFound) {
                        auto parts = static_components(detail::ServerRequestAccess::EncodedPath(request));
                        std::shared_ptr<StaticMount const> selected;
                        if (parts) {
                            for (auto const& mount : owner.m_mounts) {
                                auto const& prefix = mount->data->prefix;
                                if (parts->size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), parts->begin())
                                    && (!selected || prefix.size() > selected->data->prefix.size())) { selected = mount; }
                            }
                        }
                        if (selected) { route.handler = selected->handler; route.lazy = true; }
                    }
                    detail::ServerRequestAccess::SetPathParams(request, std::move(route.match.params));
                }
            } catch (...) {
                route.failure = std::current_exception();
                route.handled = true;
            }
            rethrowCancellation(route.failure);
            co_return route;
        }
    };

} // namespace mhttp::impl
