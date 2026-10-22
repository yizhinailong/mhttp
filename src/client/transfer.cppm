/**
 * @file transfer.cppm
 * @brief Define owned asynchronous response consumers and transfer progress callbacks without transport types.
 */
export module mhttp:client.transfer;

import std;
export import :core.task;
export import :core.function;
export import :client.response;

export namespace mhttp::client {

    /**
     * @brief Identify decoded application bytes uploaded or downloaded in one redirect hop.
     */
    enum class TransferDirection {
        Upload, ///< Request body bytes whose complete segment was written.
        Download, ///< Response body bytes accepted by the receiver or buffered by the client.
    };

    /**
     * @brief Own one monotonic progress sample; framing bytes and intermediate redirect bodies are excluded.
     */
    struct TransferProgress {
        TransferDirection direction; ///< Upload or final-response download.
        std::uint64_t transferred{}; ///< Completed decoded bytes in this direction and hop.
        std::optional<std::uint64_t> total; ///< Declared length, zero for bodyless input, unknown for chunked/EOF.
        std::size_t hop{}; ///< Zero-based redirect hop; counters restart for each upload.
        bool complete{}; ///< Entire body and framing completed, distinct from reaching the declared byte count.
    };

    /**
     * @brief Own callbacks for one Send, awaiting each callback to apply backpressure and contain its lifetime.
     * @note Callbacks run on the client strand and may use Sleep/Offload. Return an error to abort and
     *       close the connection; exceptions become Unknown. Stop, runtime cancellation, and the selected
     *       timeout policy cancel callback awaits and drain their frames. Same-client nested Sends return Busy.
     *       Headers/body callbacks receive only the final response after automatic redirects. Returned
     *       metadata owns its validated trailers; a headers snapshot has no body/trailers or reuse permission.
     */
    struct TransferCallbacks {
        MoveOnlyFunction<Task<Result<void>>(Response const&)> on_headers; ///< Borrowed snapshot through this await only.
        MoveOnlyFunction<Task<Result<void>>(std::string)> on_body; ///< Owned nonempty decoded chunk; presence disables body buffering.
        MoveOnlyFunction<Task<Result<void>>(TransferProgress)> on_progress; ///< Initial, segment, and completed samples; return an error to cancel.
    };

} // namespace mhttp::client
