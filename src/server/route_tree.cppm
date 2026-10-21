/**
 * @file route_tree.cppm
 * @brief Match HTTP methods and decoded path segments with deterministic priority and backtracking.
 */
export module mhttp:server.route_tree;

import std;
export import :server.route_pattern;
export import :core.method;
export import :core.status;
export import :core.query;

export namespace mhttp::detail {

    /**
     * @brief Own a selected handler identifier and captures, or a 404/405 outcome.
     * @internal
     */
    struct RouteMatch {
        std::optional<std::size_t> handler; ///< Opaque caller-owned handler identifier on success.
        QueryParams params; ///< Owned case-sensitive captures in path order.
        Status status{ Status::NotFound }; ///< Ok, NotFound, or MethodNotAllowed.
        std::string allow; ///< Sorted unique method tokens for a 405 response, including implicit HEAD.
        bool head_fallback{}; ///< A HEAD request selected GET after all explicit HEAD candidates failed.
    };

    /**
     * @brief Own a transport-independent segment tree with explicit matching frames and flat node ownership.
     * @note Complete registration before concurrent matching. Mutation requires external synchronization;
     *       const Match calls may run concurrently. Handler identifiers refer to storage owned by the caller.
     * @internal
     */
    class RouteTree {
    private:
        /**
         * @brief Store an edge, method bindings, and indices into flat node ownership.
         */
        struct Node {
            RouteSegment edge; ///< Edge used to reach this node; unused on the root.
            std::unordered_map<Method, std::size_t> handlers; ///< Method-specific opaque identifiers.
            std::unordered_map<std::string, std::size_t> static_children; ///< Exact byte lookup.
            std::vector<std::size_t> regex_children; ///< Regular expressions in registration order.
            std::vector<std::size_t> param_children; ///< Named parameters in registration order.
            std::optional<std::size_t> wildcard_child; ///< Terminal remainder edge.
        };

        /**
         * @brief Try higher-priority edge categories before the next category or backtracking.
         */
        enum class MatchStage {
            Static, ///< Exact bytes first.
            Regex, ///< Then regular expressions.
            Parameter, ///< Then unqualified named segments.
            Wildcard, ///< Finally all remaining segments.
            Done, ///< Restore captures and return to the parent frame.
        };

        /**
         * @brief Resume a depth-first search without consuming the native call stack.
         */
        struct MatchFrame {
            std::size_t node; ///< Index into flat node ownership.
            std::size_t index; ///< Next subject segment.
            std::size_t captures; ///< Number of captures inherited when entering this node.
            MatchStage stage{ MatchStage::Static }; ///< Next edge category to try.
            std::size_t child_index{}; ///< Next candidate within the current ordered category.
        };

        std::deque<Node> m_nodes; ///< Flat ownership also makes deeply nested tree destruction iterative.
        RouteLimits m_limits; ///< Admission and matching limits fixed for this tree.

    public:
        /**
         * @brief Create an empty routing tree with immutable regular-expression bounds.
         * @param limits Expression source and decoded subject limits.
         */
        explicit RouteTree(RouteLimits limits = {}) : m_nodes(1), m_limits{ limits } {}

        /**
         * @brief Register one method, replacing the identifier for the same exact pattern and method.
         * @param method Supported method; invalid enum values are rejected.
         * @param path Decoded route pattern; validated completely before insertion.
         * @param handler Opaque identifier whose lifetime is managed by the caller.
         * @return Success or InvalidRequest, preserving existing registrations on invalid input.
         */
        [[nodiscard]] auto Insert(
            Method method,
            std::string_view path,
            std::size_t handler
        ) -> Result<void> {
            if (ToString(method).empty()) { return std::unexpected{ Error::InvalidRequest }; }
            auto segments = parse_route_pattern(path, m_limits);
            if (!segments) { return std::unexpected{ segments.error() }; }
            std::size_t node{};
            for (auto& segment : *segments) { node = findOrInsert(node, std::move(segment)); }
            m_nodes[node].handlers.insert_or_assign(method, handler);
            return {};
        }

        /**
         * @brief Select by method and encoded path, retaining separator boundaries before decoding.
         * @param method Case-sensitive wire method; unknown methods can still produce a 405.
         * @param encoded_path Origin-form path without a query or fragment, or the literal * target.
         * @return Owned captures on success, a union of allowed methods on 405, otherwise 404.
         * @note Priority is static, regex, parameter, wildcard, with registration order breaking ties.
         *       A path or method miss backtracks. Explicit HEAD is searched before falling back to GET.
         */
        [[nodiscard]] auto Match(
            std::string_view method,
            std::string_view encoded_path
        ) const -> RouteMatch {
            RouteMatch result;
            if ((!encoded_path.starts_with('/') && encoded_path != "*")
                || encoded_path.find_first_of("?#") != std::string_view::npos) { return result; }
            auto const segments = decode_path_segments(encoded_path);
            std::set<std::string_view> allows;
            bool matched = matchNodes(ParseMethod(method), segments, allows, result);
            if (!matched && method == "HEAD") {
                matched = matchNodes(Method::Get, segments, allows, result);
                result.head_fallback = matched;
            }
            if (matched) { result.status = Status::Ok; return result; }
            if (!allows.empty()) {
                result.status = Status::MethodNotAllowed;
                for (auto token : allows) {
                    if (!result.allow.empty()) { result.allow += ", "; }
                    result.allow += token;
                }
            }
            return result;
        }

        /**
         * @brief Match using a supported method enumeration.
         * @param method Method selecting a handler.
         * @param encoded_path Encoded path without a query or fragment.
         * @return Owned selection or 404/405 outcome.
         */
        [[nodiscard]] auto Match(Method method, std::string_view encoded_path) const -> RouteMatch {
            return Match(ToString(method), encoded_path);
        }

    private:
        /**
         * @brief Reuse an identical edge or append flat ownership before linking it to its parent.
         * @param parent Existing parent node index.
         * @param segment Owned validated edge and optional compiled expression.
         * @return Existing or newly inserted child index.
         */
        auto findOrInsert(std::size_t parent, RouteSegment segment) -> std::size_t {
            auto& node = m_nodes[parent];
            if (segment.kind == RouteSegmentKind::Static) {
                auto found = node.static_children.find(segment.key);
                if (found != node.static_children.end()) { return found->second; }
            } else if (segment.kind == RouteSegmentKind::Wildcard) {
                if (node.wildcard_child) { return *node.wildcard_child; }
            } else {
                auto const& children = segment.kind == RouteSegmentKind::Regex ? node.regex_children : node.param_children;
                for (auto child : children) {
                    if (m_nodes[child].edge.key == segment.key) { return child; }
                }
            }
            auto const child = m_nodes.size();
            m_nodes.push_back(Node{ .edge = std::move(segment) });
            switch (m_nodes[child].edge.kind) {
            case RouteSegmentKind::Static: node.static_children.emplace(m_nodes[child].edge.key, child); break;
            case RouteSegmentKind::Regex: node.regex_children.push_back(child); break;
            case RouteSegmentKind::Parameter: node.param_children.push_back(child); break;
            case RouteSegmentKind::Wildcard: node.wildcard_child = child; break;
            }
            return child;
        }

        /**
         * @brief Try all eligible path branches with explicit frames and restore captures on every miss.
         * @param method Known method, or nullopt for an unsupported wire token.
         * @param segments Owned decoded subject segments, borrowed throughout matching.
         * @param allows Accumulated method tokens from complete matching paths.
         * @param result Selection populated only after finding a complete path with the requested method.
         * @return Whether a handler was selected.
         */
        auto matchNodes(
            std::optional<Method> method,
            std::vector<std::string> const& segments,
            std::set<std::string_view>& allows,
            RouteMatch& result
        ) const -> bool {
            std::vector<MatchFrame> frames{ { 0, 0, 0 } };
            std::vector<std::pair<std::string, std::string>> captures;
            while (!frames.empty()) {
                auto& frame = frames.back();
                auto const& node = m_nodes[frame.node];
                captures.resize(frame.captures);
                if (frame.index == segments.size()) {
                    for (auto const& [allowed, handler] : node.handlers) {
                        allows.insert(ToString(allowed));
                        if (allowed == Method::Get) { allows.insert("HEAD"); }
                    }
                    auto const found = method ? node.handlers.find(*method) : node.handlers.end();
                    if (found != node.handlers.end()) {
                        result.handler = found->second;
                        for (auto const& [name, value] : captures) { result.params.Add(name, value); }
                        return true;
                    }
                    frames.pop_back();
                    continue;
                }
                auto const& subject = segments[frame.index];
                switch (frame.stage) {
                case MatchStage::Static: {
                    frame.stage = MatchStage::Regex;
                    auto const found = node.static_children.find(subject);
                    if (found != node.static_children.end()) {
                        frames.push_back({ found->second, frame.index + 1, captures.size() });
                    }
                    break;
                }
                case MatchStage::Regex: {
                    bool descended{};
                    if (subject.size() <= m_limits.regex_subject_bytes) {
                        while (frame.child_index < node.regex_children.size()) {
                            auto const child = node.regex_children[frame.child_index++];
                            bool matches{};
                            try { matches = std::regex_match(subject.begin(), subject.end(), *m_nodes[child].edge.pattern); }
                            catch (std::regex_error const&) { continue; } // A runtime regex resource failure is a miss.
                            if (!matches) { continue; }
                            captures.emplace_back(m_nodes[child].edge.name, subject);
                            frames.push_back({ child, frame.index + 1, captures.size() });
                            descended = true;
                            break;
                        }
                    }
                    if (!descended) { frame.stage = MatchStage::Parameter; frame.child_index = 0; }
                    break;
                }
                case MatchStage::Parameter: {
                    if (frame.child_index < node.param_children.size()) {
                        auto const child = node.param_children[frame.child_index++];
                        captures.emplace_back(m_nodes[child].edge.name, subject);
                        frames.push_back({ child, frame.index + 1, captures.size() });
                    } else { frame.stage = MatchStage::Wildcard; }
                    break;
                }
                case MatchStage::Wildcard: {
                    frame.stage = MatchStage::Done;
                    if (node.wildcard_child) {
                        std::string remainder;
                        for (auto index = frame.index; index < segments.size(); ++index) {
                            if (index != frame.index) { remainder += '/'; }
                            remainder += segments[index];
                        }
                        captures.emplace_back("*", std::move(remainder));
                        frames.push_back({ *node.wildcard_child, segments.size(), captures.size() });
                    }
                    break;
                }
                case MatchStage::Done: frames.pop_back(); break;
                }
            }
            return false;
        }
    };

} // namespace mhttp::detail
