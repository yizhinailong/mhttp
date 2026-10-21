/**
 * @file callback.cppm
 * @brief Bound one awaited user callback by its transfer budget and all inherited cancellation sources.
 */
module mhttp:client.impl.callback;

import std;
import asio;
import :core.impl.bridge;

namespace mhttp::impl {

    /**
     * @brief Own cancellation and deadline state until an entire user Task tree has drained.
     * @note Mutable state uses the client strand. Timers and parent cancellation use weak ownership;
     *       retirement prevents a late callback from affecting a successor transfer.
     */
    class ClientCallback : public std::enable_shared_from_this<ClientCallback> {
    private:
        asio::steady_timer m_timer; ///< Optional deadline on the client executor.
        std::stop_source m_stop; ///< Combined caller/client/runtime/deadline cancellation for the Task tree.
        std::optional<Error> m_error; ///< First terminal cause, preferred over cancellation caused by expiry.
        bool m_retired{}; ///< No callback or timer may change this completed operation.

    public:
        /**
         * @brief Create inactive callback state on the client strand.
         * @param executor Serialized executor used for the Task, timer, and cancellation.
         */
        explicit ClientCallback(asio::any_io_executor executor) : m_timer{ executor } {}

        /**
         * @brief Cancel active user awaits while preserving the original cause.
         * @param error Deadline classification or explicit Canceled; call only on the client strand.
         */
        auto Cancel(Error error = Error::Canceled) -> void {
            if (m_retired || m_error) { return; }
            m_error = error;
            m_stop.request_stop();
        }

        /**
         * @brief Run an owned user Task under a remaining phase, overall, step, or unlimited budget.
         * @tparam T Owned result value, including void.
         * @param task Owned lazy callback operation; its callable is retained by the awaiting Send frame.
         * @param budget Remaining budget, or nullopt for Never.
         * @param timeout_error ReadTimeout or WriteTimeout for this callback's direction.
         * @param runtime Runtime kept alive through Offload and Task cleanup.
         * @return User result, timeout/cancellation, or Unknown for an exception; every frame has drained.
         */
        template <typename T>
        auto Run(
            Task<Result<T>> task,
            std::optional<std::chrono::steady_clock::duration> budget,
            Error timeout_error,
            std::shared_ptr<RuntimeImpl> runtime
        ) -> asio::awaitable<Result<T>> {
            auto parent = co_await asio::this_coro::cancellation_state;
            auto slot = parent.slot();
            if (slot.is_connected()) {
                slot.assign([weak = weak_from_this()](auto) {
                    if (auto owner = weak.lock()) { owner->Cancel(); }
                });
            }
            if (parent.cancelled() != asio::cancellation_type::none) { Cancel(); }
            std::optional<std::chrono::steady_clock::time_point> until;
            if (budget) {
                until = std::chrono::steady_clock::now() + *budget;
                if (*budget <= std::chrono::steady_clock::duration::zero()) { Cancel(timeout_error); }
                else {
                    m_timer.expires_at(*until);
                    m_timer.async_wait([weak = weak_from_this(), timeout_error](asio::error_code error) {
                        if (!error) { if (auto owner = weak.lock()) { owner->Cancel(timeout_error); } }
                    });
                }
            }
            Result<T> result = std::unexpected{ Error::Unknown };
            if (!m_error) {
                try { result = co_await AwaitTask(std::move(task), m_timer.get_executor(), m_stop.get_token(), std::move(runtime)); }
                catch (std::system_error const& error) {
                    if (error.code() == Error::Canceled) { Cancel(); }
                } catch (...) {}
            }
            if (until && std::chrono::steady_clock::now() >= *until) { Cancel(timeout_error); }
            m_retired = true;
            m_timer.cancel();
            if (slot.is_connected()) { slot.clear(); }
            if (m_error) { co_return std::unexpected{ *m_error }; }
            co_return result;
        }
    };

} // namespace mhttp::impl
