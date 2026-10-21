/**
 * @file response_policy.cppm
 * @brief Finalize bounded buffered wire output using request, routing, and connection lifecycle policy.
 */
export module mhttp:server.response_policy;

import std;
import :utils.date;
export import :server.options;
export import :server.routing;
export import :core.serializer;

export namespace mhttp::detail {

    /**
     * @brief Describe session facts that can prohibit another request on the same connection.
     * @internal
     */
    struct ResponseSessionState {
        std::size_t completed_requests{ 1 }; ///< Count including the response being prepared; starts at one.
        bool body_complete{ true }; ///< False when framing bytes remain unread or parsing failed.
        bool stopping{}; ///< Server shutdown has started; this response must be the last one.
    };

    /**
     * @brief Own bounded buffered output or a finalized streaming head independently of application storage.
     * @internal
     */
    struct SerializedServerResponse {
        std::string bytes; ///< Complete buffered response or streaming head for one owned asynchronous write.
        bool keep_alive{}; ///< True only when every request, response, and session policy permits reuse.
        bool streaming{}; ///< Invoke the owned provider only when wire body semantics allow it.
        BodyKind body_kind{ BodyKind::None }; ///< Chunked or EOF mode for active streams.
        Headers stream_headers; ///< Final initial fields for validating trailers, empty for buffered output.
    };

    /**
     * @brief Add fallback fields and Date, reframe the actual body, and enforce final session policy.
     * @param request Validated request metadata borrowed only for this call.
     * @param response Final application response, preserved without mutation.
     * @param options Configuration snapshot with positive finite bounds.
     * @param routing Routing policy; failed post-routing forbids preparing any response.
     * @param session Completion count, body consumption, and stop state.
     * @param now Instant used for an automatically supplied Date; ignored when Date is already present.
     * @return Owned bounded response/head, InvalidRequest, or BodyTooLarge before a write can start.
     * @note Defaults preserve repeated fields and never replace explicit fields. Status 1xx cannot be
     *       a final buffered response. HEAD/204/304 suppress body bytes before applying the wire bound.
     * @internal
     */
    [[nodiscard]] auto serialize_server_response(
        server::Request const& request,
        server::Response const& response,
        server::ServerOptions const& options,
        RoutingOutcome const& routing = {},
        ResponseSessionState const& session = {},
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
    ) -> Result<SerializedServerResponse> {
        if (!validate_server_options(options) || !routing.send_response || session.completed_requests == 0
            || IsInformational(response.Status())) {
            return std::unexpected{ Error::InvalidRequest };
        }
        auto head = ServerResponseAccess::Head(response, request.Version());
        for (auto const& [name, value] : options.default_headers) {
            if (!response.Headers().Has(name)) { static_cast<void>(head.headers.Add(name, value)); }
        }
        if (!head.headers.Has("Date")) { static_cast<void>(head.headers.Add("Date", FormatHttpDate(now))); }
        bool const streaming = response.IsStreaming();
        bool const closing_stream = streaming && request.Version() == Version::Http10
            && response.Status() != Status::NoContent && response.Status() != Status::NotModified;
        bool const keep_alive = !closing_stream && request.KeepAlive() && !routing.close && session.body_complete
            && !session.stopping && session.completed_requests < options.keep_alive_max_requests;
        if (keep_alive && request.Version() == Version::Http10
            && !has_header_token(head.headers, "Connection", "close")
            && !has_header_token(head.headers, "Connection", "keep-alive")) {
            static_cast<void>(head.headers.Add("Connection", "keep-alive"));
        }
        auto serialized = serialize_response_head(head, request.Method().value_or(Method::Get),
            { .body_bytes = response.Body().size(), .chunked = streaming && request.Version() == Version::Http11,
                .keep_alive = keep_alive, .close_delimited = streaming && request.Version() == Version::Http10 });
        if (!serialized) { return std::unexpected{ serialized.error() }; }
        if (serialized->bytes.size() > options.max_response_bytes
            || (serialized->send_body && response.Body().size() > options.max_response_bytes - serialized->bytes.size())) {
            return std::unexpected{ Error::BodyTooLarge };
        }
        if (serialized->send_body && !streaming) { serialized->bytes.append(response.Body()); }
        bool const send_stream = streaming && serialized->send_body;
        return SerializedServerResponse{ .bytes = std::move(serialized->bytes), .keep_alive = serialized->keep_alive,
            .streaming = send_stream, .body_kind = serialized->body_kind,
            .stream_headers = send_stream ? std::move(head.headers) : Headers{} };
    }

} // namespace mhttp::detail
