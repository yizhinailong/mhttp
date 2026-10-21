/**
 * @file content_provider.cppm
 * @brief Define an owned offset-based asynchronous upload source with bounded requests and replay semantics.
 */
export module mhttp:client.content_provider;

import std;
export import :core.task;
export import :core.error;

export namespace mhttp::client {

    /**
     * @brief Produce an owned binary chunk for the requested decoded offset and positive maximum size.
     * @note Return an empty string at EOF. Known-length uploads must provide exactly that length;
     *       the client does not call again once it has sent the declared length. Unknown-length
     *       uploads use chunked framing and call until EOF, including an EOF probe at the total limit.
     *       The callable is owned by Request/Send and awaited serially on the client strand; use Offload
     *       for blocking work. It must support offset zero again when 307/308 redirect replay is enabled.
     *       Capture owned state; temporary coroutine lambda captures and borrowed handler locals must
     *       not escape their lifetime. Stop, cancellation, and write budgets apply to every provider await.
     */
    using ContentProvider = std::move_only_function<Task<Result<std::string>>(std::uint64_t, std::size_t)>;

} // namespace mhttp::client
