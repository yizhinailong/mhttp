/**
 * @file stream_writer.cppm
 * @brief Expose owned asynchronous response segments and terminal trailers without transport types.
 */
export module mhttp:server.stream_writer;

import std;
import :server.impl.stream_writer;
import :core.impl.bridge;
export import :core.task;
export import :core.headers;

export namespace mhttp::detail {
    struct StreamWriterAccess;
} // namespace mhttp::detail

export namespace mhttp::server {

    /**
     * @brief Borrow a session-owned streaming window for an awaited content provider.
     * @note Write and Finish own their inputs at issuance and are lazy. Await each operation before
     *       issuing the next; no application queue is accumulated. The writer must not escape its
     *       provider. Issued Tasks own internal state but fail after the provider window retires.
     */
    class StreamWriter {
    private:
        std::shared_ptr<impl::StreamWriterImpl> m_impl; ///< Owned bounded stream state, not request storage.
        friend struct detail::StreamWriterAccess;
        /**
         * @brief Retain a nonnull active session stream without exposing backend state.
         * @param state Owned state, used only during the enclosing provider's lifetime.
         */
        explicit StreamWriter(
            std::shared_ptr<impl::StreamWriterImpl> state
        ) : m_impl{ std::move(state) } {}

    public:
        /**
         * @brief Inspect the maximum safe decoded segment for the selected framing and server limits.
         * @return Immutable bound; producers can split larger inputs before calling Write.
         */
        [[nodiscard]] auto MaxWriteSize() const noexcept -> std::size_t { return m_impl->MaxWriteSize(); }

        /**
         * @brief Prevent copying a borrowed provider window.
         * @param other Unused source.
         */
        StreamWriter(StreamWriter const& other) = delete;
        /**
         * @brief Prevent replacing or moving the session's borrowed writer.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(StreamWriter const& other) -> StreamWriter& = delete;

        /**
         * @brief Write one owned binary segment, awaiting transport completion and backpressure.
         * @param bytes Owned content; empty writes do not terminate a stream.
         * @return Lazy operation with success, Busy, BodyTooLarge, cancellation, timeout, or I/O error.
         * @note Each raw segment must fit stream_chunk_bytes, and its framed bytes must fit
         *       max_response_bytes. The first transfer failure aborts the response and cannot be ignored.
         */
        [[nodiscard]] auto Write(std::string bytes) -> Task<Result<void>> {
            return impl::ToTask(m_impl->Executor(), m_impl->Write(std::move(bytes)));
        }

        /**
         * @brief Finish once with optional bounded safe trailers; successful provider return also finishes.
         * @param trailers Owned validated fields; only chunked streams accept trailers.
         * @return Lazy operation; unsafe fields, oversized terminal bytes, or repeated completion fail.
         * @note No body writes are permitted after completion. Await this operation before returning
         *       from the provider. The session closes EOF output after the provider returns.
         */
        [[nodiscard]] auto Finish(Headers trailers = {}) -> Task<Result<void>> {
            return impl::ToTask(m_impl->Executor(), m_impl->Finish(std::move(trailers)));
        }
    };

    /**
     * @brief Produce response bytes after final hooks and header transmission, retaining owned captures.
     * @note The writer, request/response references captured by a handler, and their views may only
     *       be used until this complete Task returns. Keep CPU/blocking work off I/O execution.
     *       Own captured handler-local data; those local variables die before provider execution.
     *       Response metadata is already committed and must not be mutated by provider code.
     *       The session adds an empty terminator on success if Finish was not explicitly awaited.
     */
    using ContentProvider = std::move_only_function<Task<Result<void>>(StreamWriter&)>;

} // namespace mhttp::server

export namespace mhttp::detail {

    /**
     * @brief Construct the borrowed public writer solely for an active session provider window.
     * @internal
     */
    struct StreamWriterAccess {
        /**
         * @brief Retain initialized stream state in a stable, noncopyable public wrapper.
         * @param state Nonnull owned implementation created on its connection strand.
         * @return Writer whose reference remains valid until the awaited provider completes.
         */
        [[nodiscard]] static auto Create(
            std::shared_ptr<impl::StreamWriterImpl> state
        ) -> server::StreamWriter { return server::StreamWriter{ std::move(state) }; }
    };

} // namespace mhttp::detail
