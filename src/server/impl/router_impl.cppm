/**
 * @file router_impl.cppm
 * @brief Own guarded route configuration and prepared callback lifetimes independently of server I/O.
 */
module mhttp:server.impl.router;

import std;
import :server.routing;

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

        std::mutex m_mutex; ///< Protect configuration, freeze state, and reader registration.
        detail::RouteTree m_tree; ///< Immutable while frozen or any configuration reader exists.
        std::unordered_map<std::size_t, std::shared_ptr<detail::HttpHandler const>> m_handlers; ///< Owned bindings.
        std::vector<detail::HttpMiddleware> m_middleware; ///< Global aspects, immutable while readers exist.
        std::shared_ptr<detail::HttpHandler const> m_not_found; ///< Optional 404 handler with its own aspects.
        std::shared_ptr<detail::HttpHandler const> m_error_handler; ///< Optional final error response transformation.
        std::shared_ptr<detail::HttpDecision const> m_pre_routing; ///< Optional interception before path selection.
        std::shared_ptr<detail::HttpHandler const> m_post_routing; ///< Optional final response mutation hook.
        std::size_t m_next_id{}; ///< Next opaque binding identifier.
        std::size_t m_readers{}; ///< Prepared operations, including Tasks not yet started.
        bool m_frozen{}; ///< Server lifecycle rejects mutation until all sessions drain and thaw succeeds.

    public:
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
         * @return Success, AlreadyRunning while frozen, Busy while prepared readers exist, or InvalidRequest.
         */
        auto Register(
            Method method,
            std::string_view path,
            detail::HttpHandler handler
        ) -> Result<void> {
            auto incoming = std::make_shared<detail::HttpHandler const>(std::move(handler));
            std::shared_ptr<detail::HttpHandler const> retired;
            std::lock_guard lock{ m_mutex };
            if (m_frozen) { return std::unexpected{ Error::AlreadyRunning }; }
            if (m_readers != 0) { return std::unexpected{ Error::Busy }; }
            auto const id = m_next_id;
            m_handlers.emplace(id, incoming);
            try {
                auto binding = m_tree.Insert(method, path, id);
                if (!binding) {
                    m_handlers.erase(id);
                    return std::unexpected{ binding.error() };
                }
                if (binding->has_value()) {
                    auto previous = m_handlers.find(**binding);
                    retired = std::move(previous->second);
                    m_handlers.erase(previous);
                }
                ++m_next_id;
            } catch (...) {
                m_handlers.erase(id);
                throw;
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
            auto lease = std::make_shared<Lease>(shared_from_this());
            {
                std::lock_guard lock{ m_mutex };
                ++m_readers;
                lease->active = true;
            }
            return prepare(std::move(lease), request, response);
        }

        /**
         * @brief Wrap route execution or default 404/405 generation in ordered global aspects.
         * @param route Owned callback and configuration lease, retained in the coroutine frame.
         * @param request Borrowed until the complete handler/aspect chain finishes.
         * @param response Borrowed for response mutation through the same lifetime.
         * @note Exceptions propagate to the session boundary. Keep another prepared lease through writes
         *       when execution completes before the session releases its final route reference.
         */
        static auto Process(
            detail::PreparedRoute route,
            server::Request& request,
            server::Response& response
        ) -> Task<void> {
            auto& owner = *std::static_pointer_cast<Lease>(route.lifetime)->owner;
            if (!route.handled) {
                bool proceed{ true };
                for (auto const& middleware : owner.m_middleware) {
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
                    if (response.Status() == Status::NotFound && owner.m_not_found) {
                        co_await (*owner.m_not_found)(request, response);
                    }
                }
                for (auto const& middleware : owner.m_middleware) {
                    if (!co_await middleware.after(request, response)) { break; }
                }
            }
            if (static_cast<unsigned>(response.Status()) >= 400 && owner.m_error_handler) {
                co_await (*owner.m_error_handler)(request, response);
            }
            if (owner.m_post_routing) { co_await (*owner.m_post_routing)(request, response); }
        }

        /**
         * @brief Consume preparation and execution as one lazy operation for in-process routing.
         * @param pending Owned preparation Task that already holds its configuration lease.
         * @param request Borrowed through completion.
         * @param response Borrowed through completion.
         */
        static auto Dispatch(
            Task<detail::PreparedRoute> pending,
            server::Request& request,
            server::Response& response
        ) -> Task<void> {
            auto route = co_await std::move(pending);
            co_await Process(std::move(route), request, response);
        }

    private:
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
            detail::ServerRequestAccess::SetPathParams(request, {});
            if (owner.m_pre_routing && co_await (*owner.m_pre_routing)(request, response)) {
                co_return detail::PreparedRoute{ .lifetime = std::move(lease), .handled = true };
            }
            auto match = owner.m_tree.Match(request.MethodString(), detail::ServerRequestAccess::EncodedPath(request));
            std::shared_ptr<detail::HttpHandler const> handler;
            if (match.handler) { handler = std::as_const(owner.m_handlers).at(*match.handler); }
            detail::ServerRequestAccess::SetPathParams(request, std::move(match.params));
            co_return detail::PreparedRoute{ std::move(match), std::move(handler), std::move(lease) };
        }
    };

} // namespace mhttp::impl
