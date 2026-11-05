/**
 * @file router.cppm
 * @brief Provide the route table: path matching, per-route and global middleware, static mounts.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/verb.hpp>

export module mhttp:router;

import std;
import :config;
import :strings;
import :handler_traits;
import :request;
import :response;
import :mount_point;
import :websocket_conn;

export namespace mhttp {

    /**
     * @brief Coroutine handler of one route.
     */
    using HttpHandler = std::function<net::awaitable<void>(Request&, Response&)>;

    /**
     * @brief Middleware hook; returning false stops the chain.
     */
    using MiddlewareHandler = std::function<net::awaitable<bool>(Request&, Response&)>;

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Longest accepted route regex; long patterns cost at registration and at match time.
     */
    inline constexpr std::size_t REGEX_PATTERN_MAX{ 256 };

    /**
     * @brief Longest path segment tested against a route regex; longer segments simply do not match.
     */
    inline constexpr std::size_t REGEX_SUBJECT_MAX{ 1024 };

    /**
     * @brief Reject classic catastrophic-backtracking shapes at registration time.
     *
     * std::regex backtracks and the matched segment is client-controlled, so nested
     * quantifiers like "(a+)+" and quantified alternations like "(a|aa)+" are refused. This is
     * a heuristic; REGEX_SUBJECT_MAX bounds the input size of the shapes it misses.
     *
     * @param pattern Regex source.
     * @return True when the pattern looks dangerous.
     */
    auto has_redos_shape(std::string_view pattern) -> bool {
        std::vector<bool> groups;
        bool in_class{ false };
        for (std::size_t i{ 0 }; i < pattern.size(); ++i) {
            auto const c{ pattern[i] };
            if (c == '\\') {
                ++i;
                continue;
            }
            if (in_class) {
                in_class = c != ']';
                continue;
            }
            switch (c) {
                case '[':
                    in_class = true;
                    break;
                case '(':
                    groups.push_back(false);
                    break;
                case ')': {
                    if (groups.empty()) {
                        return false; // Unbalanced; std::regex reports it.
                    }
                    bool const risky{ groups.back() };
                    groups.pop_back();
                    if (risky && i + 1 < pattern.size() && (pattern[i + 1] == '*' || pattern[i + 1] == '+' || pattern[i + 1] == '{')) {
                        return true;
                    }
                    break;
                }
                case '|':
                case '*':
                case '+':
                case '{':
                    if (!groups.empty()) {
                        groups.back() = true;
                    }
                    break;
                default:
                    break;
            }
        }
        return false;
    }

    /**
     * @brief Path trie with static, regex, parameter, and wildcard children.
     *
     * No locking: routes are registered before the server runs and only read afterwards. The
     * server calls Reset() after every session drained on stop.
     */
    class RouteTable {
    public:
        struct HandlerEntry {
            HttpHandler handler; ///< Route handler wrapped with global middleware.
            bool lazy{ false };  ///< The handler reads the body itself.
        };

        struct Node {
            std::string key;                                   ///< Registered segment text.
            std::string param_name;                            ///< Parameter name for ":p" and "{p:re}".
            std::regex regex;                                  ///< Pattern of "{p:re}" nodes.
            std::map<Verb, HandlerEntry> handlers;             ///< Handlers by method.
            std::optional<WsRouteEntry> ws_handler;            ///< WebSocket callbacks or prepare factory.
            StringMap<std::unique_ptr<Node>> static_children;  ///< Literal segments.
            std::vector<std::unique_ptr<Node>> param_children; ///< ":name" segments, in order.
            std::vector<std::unique_ptr<Node>> regex_children; ///< "{name:re}" segments, in order.
            std::unique_ptr<Node> wildcard_child;              ///< Trailing "*".
        };

        struct RouteMatch {
            std::set<std::string> allows;  ///< Methods registered on the path when no handler matched.
            bool lazy{ false };            ///< Matched handler is lazy.
            Node const* node{ nullptr };   ///< Matched node; null when nothing matched.
            StringMap<std::string> params; ///< Captured path parameters.
        };

    private:
        std::unique_ptr<Node> m_root{ std::make_unique<Node>() }; ///< Trie root.
        HttpHandler m_post_routing;                               ///< Runs after every routed request.
        HttpHandler m_not_found;                                  ///< Runs when the status is 404.
        std::vector<MiddlewareHandler> m_global_before;           ///< Global "before" hooks.
        std::vector<MiddlewareHandler> m_global_after;            ///< Global "after" hooks.

    public:
        /**
         * @brief Register a handler for a method and path pattern.
         * @param verb Method.
         * @param key Pattern: literal, ":name", "{name:regex}", and a trailing "*".
         * @param handler Handler.
         * @param lazy True when the handler reads the body itself.
         * @throws std::invalid_argument For overly long or backtracking-prone regexes.
         * @throws std::regex_error For invalid regexes.
         */
        auto AddHttpHandler(
            Verb verb,
            std::string_view key,
            HttpHandler handler,
            bool lazy
        ) -> void {
            auto* const node{ insert(m_root.get(), split_path_segments(key), 0) };
            node->handlers[verb] = HandlerEntry{ wrapGlobal(std::move(handler)), lazy };
        }

        /**
         * @brief Register WebSocket callbacks independently of the HTTP methods at this path.
         */
        auto AddWsHandler(std::string_view key, WsHandlerEntry handlers) -> void {
            insert(m_root.get(), split_path_segments(key), 0)->ws_handler = WsRouteEntry{ std::move(handlers), {} };
        }

        /**
         * @brief Register a per-session factory that runs before the upgrade response is sent.
         */
        auto AddWsForward(std::string_view key, WsRouteEntry::Prepare prepare) -> void {
            insert(m_root.get(), split_path_segments(key), 0)->ws_handler = WsRouteEntry{ {}, std::move(prepare) };
        }

        /**
         * @brief Match an upgrade request and preserve its decoded path parameters.
         * @return Callback set, or no value so ordinary HTTP routing can handle the request.
         */
        [[nodiscard]] auto QueryWsHandler(Request& req) const -> std::optional<WsRouteEntry> {
            StringMap<std::string> params;
            auto const* node{ matchNodes(m_root.get(), req.DecodedSegments(), 0, params, [](Node const* candidate) { return candidate->ws_handler.has_value(); }) };
            if (!node) {
                return std::nullopt;
            }
            get_impl(req).SetPathParams(std::move(params));
            return node->ws_handler;
        }

        auto SetNotFoundHandler(HttpHandler handler) -> void {
            m_not_found = wrapGlobal(std::move(handler));
        }

        auto SetPostRoutingHandler(HttpHandler handler) -> void {
            m_post_routing = std::move(handler);
        }

        auto AddMiddleware(MiddlewareHandler before, MiddlewareHandler after) -> void {
            m_global_before.push_back(std::move(before));
            m_global_after.push_back(std::move(after));
        }

        /**
         * @brief Match a request; on a miss, collect the methods registered for its path.
         * @param req Request to match.
         * @return Match result.
         */
        [[nodiscard]] auto PreRouting(Request& req) const -> RouteMatch {
            RouteMatch match;
            auto const segments{ req.DecodedSegments() };
            auto const verb{ req.Method() };
            match.node = matchNodes(m_root.get(), segments, 0, match.params, [&](Node const* node) {
                auto const it{ node->handlers.find(verb) };
                if (it == node->handlers.end()) {
                    return false;
                }
                match.lazy = it->second.lazy;
                return true;
            });
            if (!match.node) {
                StringMap<std::string> ignored;
                matchNodes(m_root.get(), segments, 0, ignored, [&](Node const* node) {
                    for (auto const& [registered, entry] : node->handlers) {
                        match.allows.insert(std::string{ to_sv(http::to_string(registered)) });
                    }
                    return false;
                });
            }
            return match;
        }

        /**
         * @brief Store the captured parameters and run the matched handler.
         * @param match Result of PreRouting().
         * @param req Request.
         * @param resp Response.
         */
        auto ProcessRouting(
            RouteMatch const& match,
            Request& req,
            Response& resp
        ) const -> net::awaitable<void> {
            if (!match.node) {
                co_return;
            }
            get_impl(req).SetPathParams(match.params);
            if (auto const it{ match.node->handlers.find(req.Method()) }; it != match.node->handlers.end()) {
                co_await it->second.handler(req, resp);
            }
        }

        /**
         * @brief Run the not-found handler (for 404) and the post-routing handler.
         * @param req Request.
         * @param resp Response; skipped once its header was sent.
         */
        auto PostRouting(Request& req, Response& resp) const -> net::awaitable<void> {
            if (get_impl(resp).HeaderSent()) {
                co_return;
            }
            if (m_not_found && resp.Result() == Status::not_found) {
                co_await m_not_found(req, resp);
            }
            if (m_post_routing) {
                co_await m_post_routing(req, resp);
            }
        }

        /**
         * @brief Drop every route and hook, releasing what handlers captured.
         */
        auto Reset() -> void {
            m_root = std::make_unique<Node>();
            m_post_routing = nullptr;
            m_not_found = nullptr;
            m_global_before.clear();
            m_global_after.clear();
        }

    private:
        /**
         * @brief Wrap a handler with the global middleware chain.
         *
         * "after" hooks run only when the handler returned normally (or "before" stopped the
         * chain); exceptions skip them and propagate so the session answers 500. httplib catches
         * into an exception_ptr and rethrows; letting the exception propagate is equivalent and
         * avoids a copy of the exception object.
         *
         * @param handler Route handler.
         * @return Wrapped handler; it reads the hook lists at call time.
         */
        [[nodiscard]] auto wrapGlobal(HttpHandler handler) const -> HttpHandler {
            return [handler = std::move(handler), this](Request& req, Response& resp) -> net::awaitable<void> {
                bool ok{ true };
                for (auto const& before : m_global_before) {
                    if (!co_await before(req, resp)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    co_await handler(req, resp);
                }
                for (auto const& after : m_global_after) {
                    if (!co_await after(req, resp)) {
                        break;
                    }
                }
            };
        }

        /**
         * @brief Find or create the node for `segments[index..]`.
         * @param parent Node to descend from.
         * @param segments Pattern segments.
         * @param index Current segment.
         * @return Node for the whole pattern.
         */
        static auto insert(
            Node* parent,
            std::vector<std::string_view> const& segments,
            std::size_t index
        ) -> Node* {
            if (index >= segments.size()) {
                return parent;
            }
            auto const segment{ segments[index] };
            if (index == segments.size() - 1 && segment == "*") {
                if (!parent->wildcard_child) {
                    parent->wildcard_child = std::make_unique<Node>();
                    parent->wildcard_child->key = segment;
                }
                return insert(parent->wildcard_child.get(), segments, index + 1);
            }
            if (segment.starts_with(':')) {
                return insert(findOrAdd(parent->param_children, segment, [&](Node& node) { node.param_name = segment.substr(1); }), segments, index + 1);
            }
            if (segment.size() >= 2 && segment.front() == '{' && segment.back() == '}') {
                return insert(findOrAdd(parent->regex_children, segment, [&](Node& node) { initRegexNode(node, segment); }), segments, index + 1);
            }
            auto [it, inserted]{ parent->static_children.try_emplace(std::string{ segment }, nullptr) };
            if (inserted) {
                it->second = std::make_unique<Node>();
                it->second->key = segment;
            }
            return insert(it->second.get(), segments, index + 1);
        }

        /**
         * @brief Return the child with a key, creating it in registration order.
         * @param children Child list.
         * @param key Segment text.
         * @param init Initializes a new node.
         * @return Child node.
         */
        static auto findOrAdd(
            std::vector<std::unique_ptr<Node>>& children,
            std::string_view key,
            auto init
        ) -> Node* {
            auto const it{ std::ranges::find_if(children, [&](auto const& node) { return node->key == key; }) };
            if (it != children.end()) {
                return it->get();
            }
            auto node{ std::make_unique<Node>() };
            node->key = key;
            init(*node);
            children.push_back(std::move(node));
            return children.back().get();
        }

        /**
         * @brief Parse "{name:regex}" into a regex node.
         * @param node Node to fill.
         * @param segment Segment including the braces.
         * @throws std::invalid_argument For overly long or backtracking-prone regexes.
         */
        static auto initRegexNode(Node& node, std::string_view segment) -> void {
            auto const inside{ segment.substr(1, segment.size() - 2) };
            auto const colon{ inside.find(':') };
            auto const pattern{ colon == std::string_view::npos ? inside : inside.substr(colon + 1) };
            if (pattern.size() > REGEX_PATTERN_MAX) {
                throw std::invalid_argument{ std::format("mhttp: route regex pattern is too long (limit {}): {}", REGEX_PATTERN_MAX, pattern) };
            }
            if (has_redos_shape(pattern)) {
                throw std::invalid_argument{ std::format("mhttp: route regex pattern rejected, it may backtrack catastrophically (nested quantifier or quantified alternation): {}", pattern) };
            }
            node.param_name = inside.substr(0, colon);
            node.regex = std::regex{ pattern.begin(), pattern.end() };
        }

        /**
         * @brief Depth-first match: static, then regex, then parameter, then wildcard children.
         * @param parent Current node.
         * @param segments Decoded request segments.
         * @param index Current segment.
         * @param params Receives captured parameters.
         * @param accept Decides whether a terminal node matches (e.g. has the method).
         * @return Matching node, or nullptr.
         */
        auto matchNodes(
            Node const* parent,
            std::span<std::string const> segments,
            std::size_t index,
            StringMap<std::string>& params,
            std::function<bool(Node const*)> const& accept
        ) const -> Node const* {
            if (!parent) {
                return nullptr;
            }
            if (index == segments.size()) {
                return accept(parent) ? parent : nullptr;
            }
            auto const& segment{ segments[index] };

            if (auto const it{ parent->static_children.find(segment) }; it != parent->static_children.end()) {
                if (auto const* node{ matchNodes(it->second.get(), segments, index + 1, params, accept) }) {
                    return node;
                }
            }
            if (segment.size() <= REGEX_SUBJECT_MAX) {
                for (auto const& child : parent->regex_children) {
                    if (!std::regex_match(segment, child->regex)) {
                        continue;
                    }
                    params[child->param_name] = segment;
                    if (auto const* node{ matchNodes(child.get(), segments, index + 1, params, accept) }) {
                        return node;
                    }
                    params.erase(child->param_name);
                }
            }
            for (auto const& child : parent->param_children) {
                params[child->param_name] = segment;
                if (auto const* node{ matchNodes(child.get(), segments, index + 1, params, accept) }) {
                    return node;
                }
                params.erase(child->param_name);
            }
            if (parent->wildcard_child) {
                std::string rest;
                for (auto i{ index }; i < segments.size(); ++i) {
                    if (i != index) {
                        rest += '/';
                    }
                    rest += segments[i];
                }
                params["*"] = std::move(rest);
                if (auto const* node{ matchNodes(parent->wildcard_child.get(), segments, segments.size(), params, accept) }) {
                    return node;
                }
                params.erase("*");
            }
            return nullptr;
        }
    };

    /**
     * @brief Detect an aspect's Before(Request&, Response&) hook.
     */
    template <typename T>
    concept HasBefore = requires(T& aspect, Request& req, Response& resp) { aspect.Before(req, resp); };

    /**
     * @brief Detect an aspect's After(Request&, Response&) hook.
     */
    template <typename T>
    concept HasAfter = requires(T& aspect, Request& req, Response& resp) { aspect.After(req, resp); };

    /**
     * @brief Detect std::reference_wrapper, which marks a borrowed aspect.
     */
    template <typename T>
    inline constexpr bool is_reference_wrapper_v = false;

    template <typename T>
    inline constexpr bool is_reference_wrapper_v<std::reference_wrapper<T>> = true;

    /**
     * @brief Unwrap std::reference_wrapper aspects (borrowed); others are owned values.
     * @param aspect Stored aspect.
     * @return Aspect object.
     */
    template <typename T>
    auto unwrap_aspect(T& aspect) -> decltype(auto) {
        if constexpr (is_reference_wrapper_v<std::remove_cv_t<T>>) {
            return aspect.get();
        } else {
            return (aspect);
        }
    }

    /**
     * @brief Take ownership of an aspect: rvalues move in, lvalues are copied.
     * @param aspect Aspect; pass std::ref(instance) to share a non-copyable stateful one.
     * @return Shared owner of the aspect.
     */
    template <typename T>
    auto own_aspect(T&& aspect) -> std::shared_ptr<std::decay_t<T>> {
        static_assert(std::is_constructible_v<std::decay_t<T>, T&&>, "aspects are owned by value: pass std::ref(instance) to borrow a non-copyable stateful aspect, and keep it alive longer than the router");
        return std::make_shared<std::decay_t<T>>(std::forward<T>(aspect));
    }

    /**
     * @brief Run an aspect's Before hook unless the chain already stopped.
     * @param aspect Aspect.
     * @param req Request.
     * @param resp Response.
     * @param ok In: chain still running. Out: hook result.
     */
    template <typename T>
    auto do_before(T& aspect, Request& req, Response& resp, bool& ok) -> net::awaitable<void> {
        if constexpr (HasBefore<T>) {
            if (!ok) {
                co_return;
            }
            if constexpr (is_awaitable_v<decltype(aspect.Before(req, resp))>) {
                ok = co_await aspect.Before(req, resp);
            } else {
                ok = aspect.Before(req, resp);
            }
        }
    }

    /**
     * @brief Run an aspect's After hook unless the chain already stopped.
     * @param aspect Aspect.
     * @param req Request.
     * @param resp Response.
     * @param ok In: chain still running. Out: hook result.
     */
    template <typename T>
    auto do_after(T& aspect, Request& req, Response& resp, bool& ok) -> net::awaitable<void> {
        if constexpr (HasAfter<T>) {
            if (!ok) {
                co_return;
            }
            if constexpr (is_awaitable_v<decltype(aspect.After(req, resp))>) {
                ok = co_await aspect.After(req, resp);
            } else {
                ok = aspect.After(req, resp);
            }
        }
    }

    /**
     * @brief Adapt a handler and wrap it with per-route aspects.
     * @param handler Sync `void(Request&, Response&)` or coroutine handler.
     * @param aspects Aspects with optional Before/After hooks returning bool or awaitable<bool>.
     * @return Coroutine handler.
     */
    template <typename Func, typename... Aspects>
    auto make_route_handler(Func&& handler, Aspects&&... aspects) -> HttpHandler {
        HttpHandler coro_handler{ make_coro_handler<Request&, Response&>(std::forward<Func>(handler)) };
        if constexpr (sizeof...(Aspects) == 0) {
            return coro_handler;
        } else {
            // Owned at registration: storing references would capture the caller's stack objects.
            return [coro_handler = std::move(coro_handler), owned = std::make_tuple(own_aspect(std::forward<Aspects>(aspects))...)](Request& req, Response& resp) -> net::awaitable<void> {
                bool ok{ true };
                co_await std::apply([&](auto&... aspect) -> net::awaitable<void> { ((co_await do_before(unwrap_aspect(*aspect), req, resp, ok)), ...); }, owned);
                // An exception skips the After hooks and propagates, as in httplib.
                if (ok) {
                    co_await coro_handler(req, resp);
                }
                ok = true;
                co_await std::apply([&](auto&... aspect) -> net::awaitable<void> { ((co_await do_after(unwrap_aspect(*aspect), req, resp, ok)), ...); }, owned);
            };
        }
    }

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief HTTP route table with global and per-route middleware.
     *
     * Registration is configuration-phase only (before the server runs) and unsynchronized;
     * changing routes while serving is undefined behavior. After stop() drains every session the
     * server clears the table, so re-register routes before running the same server again.
     *
     * Path matching splits the raw path on literal '/' before decoding each segment, so "%2F"
     * is segment content. Register keys as decoded text (e.g. "/a b"). At each level the priority
     * is static > regex > parameter > wildcard; within a kind, registration order wins.
     *
     * Aspects passed to registration (or Use()) are owned by value; pass std::ref(instance) to
     * share a stateful, non-copyable one that outlives the router. An aspect may provide
     * Before(Request&, Response&) and/or After(Request&, Response&) returning bool or
     * net::awaitable<bool>; false from Before skips the handler and later hooks.
     */
    class Router {
    private:
        std::unique_ptr<detail::RouteTable> m_table{ std::make_unique<detail::RouteTable>() }; ///< Routes.

    public:
        Router() = default;
        Router(Router const&) = delete;
        auto operator=(Router const&) -> Router& = delete;

        /**
         * @brief Register a WebSocket route with synchronous or coroutine callbacks.
         * @param key Same path patterns and precedence as HTTP routes.
         * @param open Callable `(WebSocketConn::WeakPtr)` after a successful upgrade.
         * @param message Callable `(WebSocketConn::WeakPtr, WebSocketMessage)` per message.
         * @param close Callable `(WebSocketConn::WeakPtr)` once an established session ends.
         * @note Callbacks run on the connection strand; successful upgrades bypass HTTP middleware.
         */
        template <typename Open, typename Message, typename Close>
        auto SetWsHandler(
            std::string_view key,
            Open&& open,
            Message&& message,
            Close&& close
        ) -> void {
            m_table->AddWsHandler(key, detail::WsHandlerEntry{ detail::make_coro_handler<WebSocketConn::WeakPtr>(std::forward<Open>(open)), detail::make_coro_handler<WebSocketConn::WeakPtr, WebSocketMessage>(std::forward<Message>(message)), detail::make_coro_handler<WebSocketConn::WeakPtr>(std::forward<Close>(close)) });
        }

        /**
         * @brief Add global middleware that wraps every route and the not-found handler.
         * @param aspects Aspects to add, outermost first.
         */
        template <typename... Aspects>
        auto Use(Aspects&&... aspects) -> void {
            (useOne(std::forward<Aspects>(aspects)), ...);
        }

        /**
         * @brief Register a handler; the body is read before it runs (use Request::As*()).
         * @param verb Method.
         * @param key Path pattern: literals, ":name", "{name:regex}", trailing "*".
         * @param handler Sync or coroutine `(Request&, Response&)` callable.
         * @param aspects Per-route aspects.
         */
        template <typename Func, typename... Aspects>
        requires(!std::is_member_function_pointer_v<std::decay_t<Func>>)
        auto SetHttpHandler(
            Verb verb,
            std::string_view key,
            Func&& handler,
            Aspects&&... aspects
        ) -> void {
            m_table->AddHttpHandler(verb, key, detail::make_route_handler(std::forward<Func>(handler), std::forward<Aspects>(aspects)...), false);
        }

        /**
         * @brief Register one handler for several methods.
         * @tparam Verbs Methods, at least one.
         * @param key Path pattern.
         * @param handler Callable, copied per method.
         * @param aspects Per-route aspects, copied per method.
         */
        template <Verb... Verbs, typename Func, typename... Aspects>
        requires(sizeof...(Verbs) >= 1 && !std::is_member_function_pointer_v<std::decay_t<Func>>)
        auto SetHttpHandler(std::string_view key, Func handler, Aspects&&... aspects) -> void {
            (SetHttpHandler(Verbs, key, handler, aspects...), ...);
        }

        /**
         * @brief Register a member function of `owner` for several methods.
         * @tparam Verbs Methods, at least one.
         * @param key Path pattern.
         * @param handler Member function taking (Request&, Response&).
         * @param owner Object that must outlive the router.
         * @param aspects Per-route aspects.
         */
        template <Verb... Verbs, typename Func, typename Owner, typename... Aspects>
        requires(sizeof...(Verbs) >= 1 && std::is_member_function_pointer_v<Func>)
        auto SetHttpHandler(
            std::string_view key,
            Func handler,
            Owner& owner,
            Aspects&&... aspects
        ) -> void {
            SetHttpHandler<Verbs...>(key, bindMember(handler, owner), std::forward<Aspects>(aspects)...);
        }

        /**
         * @brief Register a lazy handler: the body is not read first (stream it or call Read*()).
         * @param verb Method.
         * @param key Path pattern.
         * @param handler Sync or coroutine `(Request&, Response&)` callable.
         * @param aspects Per-route aspects.
         */
        template <typename Func, typename... Aspects>
        requires(!std::is_member_function_pointer_v<std::decay_t<Func>>)
        auto SetLazyHttpHandler(
            Verb verb,
            std::string_view key,
            Func&& handler,
            Aspects&&... aspects
        ) -> void {
            m_table->AddHttpHandler(verb, key, detail::make_route_handler(std::forward<Func>(handler), std::forward<Aspects>(aspects)...), true);
        }

        template <Verb... Verbs, typename Func, typename... Aspects>
        requires(sizeof...(Verbs) >= 1 && !std::is_member_function_pointer_v<std::decay_t<Func>>)
        auto SetLazyHttpHandler(std::string_view key, Func handler, Aspects&&... aspects) -> void {
            (SetLazyHttpHandler(Verbs, key, handler, aspects...), ...);
        }

        template <Verb... Verbs, typename Func, typename Owner, typename... Aspects>
        requires(sizeof...(Verbs) >= 1 && std::is_member_function_pointer_v<Func>)
        auto SetLazyHttpHandler(
            std::string_view key,
            Func handler,
            Owner& owner,
            Aspects&&... aspects
        ) -> void {
            SetLazyHttpHandler<Verbs...>(key, bindMember(handler, owner), std::forward<Aspects>(aspects)...);
        }

        /**
         * @brief Handle responses that are still 404 after routing (unmatched paths included).
         * @param handler Sync or coroutine `(Request&, Response&)` callable.
         * @param aspects Per-route aspects.
         */
        template <typename Func, typename... Aspects>
        auto SetHttpNotFoundHandler(Func&& handler, Aspects&&... aspects) -> void {
            m_table->SetNotFoundHandler(detail::make_route_handler(std::forward<Func>(handler), std::forward<Aspects>(aspects)...));
        }

        /**
         * @brief Run a hook after every request whose response was not streamed.
         * @param handler Sync or coroutine `(Request&, Response&)` callable.
         */
        template <typename Func>
        auto SetPostRoutingHandler(Func&& handler) -> void {
            m_table->SetPostRoutingHandler(detail::make_coro_handler<Request&, Response&>(std::forward<Func>(handler)));
        }

        /**
         * @brief Serve a directory under a URL prefix for GET and HEAD.
         * @param mount_point URL prefix, e.g. "/static".
         * @param dir Directory to serve.
         * @param aspects Per-route aspects.
         */
        template <typename... Aspects>
        auto SetStaticMountPoint(
            std::string mount_point,
            std::filesystem::path dir,
            Aspects&&... aspects
        ) -> void {
            SetStaticMountPoint(MountPointEntry{ std::move(mount_point), std::move(dir) }, std::forward<Aspects>(aspects)...);
        }

        /**
         * @brief Serve a configured mount point for GET and HEAD.
         *
         * Registers both "/mount/*" and the bare "/mount" (which redirects to "/mount/"), so a
         * catch-all route elsewhere does not turn the bare path into a 405.
         *
         * @param entry Mount point configuration.
         * @param aspects Per-route aspects, constructed once and shared by all registrations.
         */
        template <typename... Aspects>
        auto SetStaticMountPoint(MountPointEntry entry, Aspects&&... aspects) -> void {
            std::string base{ entry.MountPoint() };
            while (base.size() > 1 && base.back() == '/') {
                base.pop_back();
            }
            bool const is_root{ base.empty() || base == "/" };
            auto const wildcard_key{ is_root ? std::string{ "/*" } : base + "/*" };
            auto const handler{ detail::make_route_handler([entry = std::move(entry)](Request& req, Response& resp) { entry(req, resp); }, std::forward<Aspects>(aspects)...) };
            m_table->AddHttpHandler(Verb::get, wildcard_key, handler, false);
            m_table->AddHttpHandler(Verb::head, wildcard_key, handler, false);
            if (!is_root) {
                m_table->AddHttpHandler(Verb::get, base, handler, false);
                m_table->AddHttpHandler(Verb::head, base, handler, false);
            }
        }

        friend auto route_table(Router& router) -> detail::RouteTable& {
            return *router.m_table;
        }

    private:
        /**
         * @brief Register one global aspect.
         * @param aspect Aspect to own (or a std::reference_wrapper to borrow).
         */
        template <typename T>
        auto useOne(T&& aspect) -> void {
            auto owned{ detail::own_aspect(std::forward<T>(aspect)) };
            m_table->AddMiddleware(
                [owned](Request& req, Response& resp) -> net::awaitable<bool> {
                    bool ok{ true };
                    co_await detail::do_before(detail::unwrap_aspect(*owned), req, resp, ok);
                    co_return ok;
                },
                [owned](Request& req, Response& resp) -> net::awaitable<bool> {
                    bool ok{ true };
                    co_await detail::do_after(detail::unwrap_aspect(*owned), req, resp, ok);
                    co_return ok;
                }
            );
        }

        /**
         * @brief Bind a member function to its owner.
         * @param handler Member function.
         * @param owner Object.
         * @return Callable `(Request&, Response&)`.
         */
        template <typename Func, typename Owner>
        static auto bindMember(Func handler, Owner& owner) {
            return [handler, &owner](Request& req, Response& resp) {
                return std::invoke(handler, owner, req, resp);
            };
        }
    };

} // namespace mhttp
