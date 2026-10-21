/**
 * @file method.cppm
 * @brief Define case-sensitive HTTP method names, including common registered extensions.
 */
export module mhttp:core.method;

import std;

export namespace mhttp {

    /**
     * @brief Identify HTTP methods independently of the transport backend.
     */
    enum class Method {
        Get, ///< GET method.
        Head, ///< HEAD method.
        Post, ///< POST method.
        Put, ///< PUT method.
        Delete, ///< DELETE method.
        Connect, ///< CONNECT method.
        Options, ///< OPTIONS method.
        Trace, ///< TRACE method.
        Patch, ///< PATCH method.
        Copy, ///< COPY method.
        Lock, ///< LOCK method.
        MkCol, ///< MKCOL method.
        Move, ///< MOVE method.
        PropFind, ///< PROPFIND method.
        PropPatch, ///< PROPPATCH method.
        Search, ///< SEARCH method.
        Unlock, ///< UNLOCK method.
        Bind, ///< BIND method.
        Rebind, ///< REBIND method.
        Unbind, ///< UNBIND method.
        Acl, ///< ACL method.
        Report, ///< REPORT method.
        MkActivity, ///< MKACTIVITY method.
        Checkout, ///< CHECKOUT method.
        Merge, ///< MERGE method.
        MSearch, ///< M-SEARCH method.
        Notify, ///< NOTIFY method.
        Subscribe, ///< SUBSCRIBE method.
        Unsubscribe, ///< UNSUBSCRIBE method.
        Purge, ///< PURGE method.
        MkCalendar, ///< MKCALENDAR method.
        Link, ///< LINK method.
        Unlink, ///< UNLINK method.
    };

    /**
     * @brief Obtain a method's canonical wire spelling.
     * @param method Method to describe.
     * @return Static method name, or an empty view for an out-of-range value.
     */
    [[nodiscard]] constexpr auto ToString(Method method) noexcept -> std::string_view {
        switch (method) {
        case Method::Get: return "GET";
        case Method::Head: return "HEAD";
        case Method::Post: return "POST";
        case Method::Put: return "PUT";
        case Method::Delete: return "DELETE";
        case Method::Connect: return "CONNECT";
        case Method::Options: return "OPTIONS";
        case Method::Trace: return "TRACE";
        case Method::Patch: return "PATCH";
        case Method::Copy: return "COPY";
        case Method::Lock: return "LOCK";
        case Method::MkCol: return "MKCOL";
        case Method::Move: return "MOVE";
        case Method::PropFind: return "PROPFIND";
        case Method::PropPatch: return "PROPPATCH";
        case Method::Search: return "SEARCH";
        case Method::Unlock: return "UNLOCK";
        case Method::Bind: return "BIND";
        case Method::Rebind: return "REBIND";
        case Method::Unbind: return "UNBIND";
        case Method::Acl: return "ACL";
        case Method::Report: return "REPORT";
        case Method::MkActivity: return "MKACTIVITY";
        case Method::Checkout: return "CHECKOUT";
        case Method::Merge: return "MERGE";
        case Method::MSearch: return "M-SEARCH";
        case Method::Notify: return "NOTIFY";
        case Method::Subscribe: return "SUBSCRIBE";
        case Method::Unsubscribe: return "UNSUBSCRIBE";
        case Method::Purge: return "PURGE";
        case Method::MkCalendar: return "MKCALENDAR";
        case Method::Link: return "LINK";
        case Method::Unlink: return "UNLINK";
        }
        return {};
    }

    /**
     * @brief Recognize a supported method using exact, case-sensitive spelling.
     * @param value Borrowed method token.
     * @return The recognized method, or nullopt for unknown or malformed input.
     */
    [[nodiscard]] constexpr auto ParseMethod(std::string_view value) noexcept -> std::optional<Method> {
        if (value == "GET") { return Method::Get; }
        if (value == "HEAD") { return Method::Head; }
        if (value == "POST") { return Method::Post; }
        if (value == "PUT") { return Method::Put; }
        if (value == "DELETE") { return Method::Delete; }
        if (value == "CONNECT") { return Method::Connect; }
        if (value == "OPTIONS") { return Method::Options; }
        if (value == "TRACE") { return Method::Trace; }
        if (value == "PATCH") { return Method::Patch; }
        if (value == "COPY") { return Method::Copy; }
        if (value == "LOCK") { return Method::Lock; }
        if (value == "MKCOL") { return Method::MkCol; }
        if (value == "MOVE") { return Method::Move; }
        if (value == "PROPFIND") { return Method::PropFind; }
        if (value == "PROPPATCH") { return Method::PropPatch; }
        if (value == "SEARCH") { return Method::Search; }
        if (value == "UNLOCK") { return Method::Unlock; }
        if (value == "BIND") { return Method::Bind; }
        if (value == "REBIND") { return Method::Rebind; }
        if (value == "UNBIND") { return Method::Unbind; }
        if (value == "ACL") { return Method::Acl; }
        if (value == "REPORT") { return Method::Report; }
        if (value == "MKACTIVITY") { return Method::MkActivity; }
        if (value == "CHECKOUT") { return Method::Checkout; }
        if (value == "MERGE") { return Method::Merge; }
        if (value == "M-SEARCH") { return Method::MSearch; }
        if (value == "NOTIFY") { return Method::Notify; }
        if (value == "SUBSCRIBE") { return Method::Subscribe; }
        if (value == "UNSUBSCRIBE") { return Method::Unsubscribe; }
        if (value == "PURGE") { return Method::Purge; }
        if (value == "MKCALENDAR") { return Method::MkCalendar; }
        if (value == "LINK") { return Method::Link; }
        if (value == "UNLINK") { return Method::Unlink; }
        return std::nullopt;
    }

} // namespace mhttp

