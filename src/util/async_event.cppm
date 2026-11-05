/**
 * @file async_event.cppm
 * @brief Provide a thread-safe, coalescing coroutine event (port of httplib::util::async_event).
 */
module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/channel_error.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:async_event;

import std;
import :config;

export namespace mhttp::util {

    /**
     * @brief Binary, coalescing notification that coroutines can await.
     *
     * The event is either NOT_SIGNALED or SIGNALED. Repeated notifications without a waiter
     * collapse into one latched notification; this is not a counting semaphore.
     *
     * NotifyOne(), NotifyAll(), TryWait(), Close(), Reset(), IsClosed(), and IsSignaled() may
     * be called from any thread. An in-flight Wait() keeps the shared state alive; destroying
     * the event closes it and wakes the waiter with WaitResult::Closed.
     */
    class AsyncEvent {
    public:
        /**
         * @brief Outcome of NotifyOne() or NotifyAll().
         */
        enum class NotifyResult {
            Notified,  ///< A waiter was woken or the event became signaled.
            Coalesced, ///< The event was already signaled; nothing changed.
            Closed,    ///< The event is closed.
        };

        /**
         * @brief Reason a Wait() or WaitFor() resumed.
         */
        enum class WaitResult {
            Notified,  ///< A notification was consumed.
            Closed,    ///< The event was closed.
            Cancelled, ///< The awaiting coroutine was cancelled.
            TimedOut,  ///< WaitFor() reached its timeout.
            Failed,    ///< Another error occurred.
        };

    private:
        class State;

        std::shared_ptr<State> m_state; ///< Shared with every in-flight wait.

    public:
        /**
         * @brief Create a non-signaled, open event.
         * @param ex Executor used for wait channels and timeout timers.
         */
        explicit AsyncEvent(net::any_io_executor ex);

        /**
         * @brief Close the event, waking pending waiters with WaitResult::Closed.
         */
        ~AsyncEvent();

        AsyncEvent(AsyncEvent const&) = delete;
        auto operator=(AsyncEvent const&) -> AsyncEvent& = delete;

        /**
         * @brief Return the executor associated with this event.
         * @return Executor passed at construction.
         */
        [[nodiscard]] auto GetExecutor() const -> net::any_io_executor;

        /**
         * @brief Wake at most one waiter, or latch the notification when none is waiting.
         * @return Whether the notification was delivered, coalesced, or rejected.
         */
        auto NotifyOne() -> NotifyResult;

        /**
         * @brief Wake every current waiter, or latch the notification when none is waiting.
         * @return Whether the notification was delivered, coalesced, or rejected.
         */
        auto NotifyAll() -> NotifyResult;

        /**
         * @brief Wait for and consume a notification.
         * @return Reason the wait resumed; never throws for close or cancellation.
         */
        [[nodiscard]] auto Wait() -> net::awaitable<WaitResult>;

        /**
         * @brief Wait for a notification, giving up after a timeout.
         * @param timeout Maximum time to wait.
         * @return Reason the wait resumed, including WaitResult::TimedOut.
         */
        [[nodiscard]] auto WaitFor(
            std::chrono::steady_clock::duration timeout
        ) -> net::awaitable<WaitResult>;

        /**
         * @brief Consume a latched notification without suspending.
         * @return True when a notification was consumed.
         */
        auto TryWait() -> bool;

        /**
         * @brief Close the event permanently and wake every waiter; idempotent.
         */
        auto Close() -> void;

        /**
         * @brief Reopen a closed event and drop any latched notification.
         * @note Call only while no wait is in flight, e.g. between two runs of the owner.
         */
        auto Reset() -> void;

        /**
         * @brief Report whether the event is closed.
         * @return Snapshot only; do not use it for synchronization.
         */
        [[nodiscard]] auto IsClosed() const noexcept -> bool;

        /**
         * @brief Report whether a notification is latched.
         * @return Snapshot only; do not use it for synchronization.
         */
        [[nodiscard]] auto IsSignaled() const -> bool;
    };

} // namespace mhttp::util

namespace mhttp::util {

    /**
     * @brief Shared state behind AsyncEvent, kept alive by every in-flight wait.
     */
    class AsyncEvent::State {
    private:
        using ChannelType = net::experimental::concurrent_channel<void(boost::system::error_code)>;

        /**
         * @brief One suspended wait operation.
         *
         * The capacity-1 channel is a one-shot wakeup latch: waking closes it. `pending` tells
         * who resumed the waiter: true means it is still queued, so cancellation resumed it and
         * it must remove itself; false means the event completed it and `result` holds why.
         */
        struct Waiter {
            ChannelType channel;                     ///< One-shot wakeup latch.
            WaitResult result{ WaitResult::Failed }; ///< Outcome set by the event.
            bool pending{ false };                   ///< Still queued in m_waiters.

            /**
             * @brief Create a waiter whose channel runs on the event's executor.
             * @param ex Executor for the channel.
             */
            explicit Waiter(net::any_io_executor ex) : channel{ std::move(ex), 1 } {}
        };

        net::any_io_executor m_executor;                 ///< Executor for channels and timers.
        mutable std::mutex m_mutex;                      ///< Guards every field below.
        std::atomic<bool> m_closed{ false };             ///< Closed flag, also read lock-free.
        bool m_signaled{ false };                        ///< Latched notification.
        std::deque<std::shared_ptr<Waiter>> m_waiters;   ///< Suspended waits in FIFO order.

    public:
        /**
         * @brief Create open, non-signaled state.
         * @param ex Executor for channels and timers.
         */
        explicit State(net::any_io_executor ex) : m_executor{ std::move(ex) } {}

        State(State const&) = delete;
        auto operator=(State const&) -> State& = delete;

        /**
         * @brief Return the executor for channels and timers.
         * @return Executor passed at construction.
         */
        [[nodiscard]] auto GetExecutor() const -> net::any_io_executor {
            return m_executor;
        }

        /**
         * @brief Wake the oldest waiter or latch the notification.
         * @return Notification outcome.
         */
        auto NotifyOne() -> NotifyResult {
            std::shared_ptr<Waiter> target;
            {
                std::lock_guard lock{ m_mutex };
                if (m_closed.load(std::memory_order_acquire)) {
                    return NotifyResult::Closed;
                }
                if (m_waiters.empty()) {
                    if (m_signaled) {
                        return NotifyResult::Coalesced;
                    }
                    m_signaled = true;
                    return NotifyResult::Notified;
                }
                target = std::move(m_waiters.front());
                m_waiters.pop_front();
                target->pending = false;
                target->result = WaitResult::Notified;
            }
            // Closing the one-shot channel wakes a pending receive, or one started later.
            target->channel.close();
            return NotifyResult::Notified;
        }

        /**
         * @brief Wake every waiter or latch the notification.
         * @return Notification outcome.
         */
        auto NotifyAll() -> NotifyResult {
            std::vector<std::shared_ptr<Waiter>> targets;
            {
                std::lock_guard lock{ m_mutex };
                if (m_closed.load(std::memory_order_acquire)) {
                    return NotifyResult::Closed;
                }
                if (m_waiters.empty()) {
                    if (m_signaled) {
                        return NotifyResult::Coalesced;
                    }
                    m_signaled = true;
                    return NotifyResult::Notified;
                }
                targets = takeAllLocked(WaitResult::Notified);
            }
            for (auto& waiter : targets) {
                waiter->channel.close();
            }
            return NotifyResult::Notified;
        }

        /**
         * @brief Consume a latched notification without suspending.
         * @return True when a notification was consumed.
         */
        auto TryWait() -> bool {
            std::lock_guard lock{ m_mutex };
            if (m_closed.load(std::memory_order_acquire) || !m_signaled) {
                return false;
            }
            m_signaled = false;
            return true;
        }

        /**
         * @brief Close permanently and wake every waiter with WaitResult::Closed.
         */
        auto Close() -> void {
            std::vector<std::shared_ptr<Waiter>> targets;
            {
                std::lock_guard lock{ m_mutex };
                if (m_closed.load(std::memory_order_acquire)) {
                    return;
                }
                m_closed.store(true, std::memory_order_release);
                m_signaled = false;
                targets = takeAllLocked(WaitResult::Closed);
            }
            for (auto& waiter : targets) {
                waiter->channel.close();
            }
        }

        /**
         * @brief Reopen and drop any latched notification.
         */
        auto Reset() -> void {
            std::lock_guard lock{ m_mutex };
            m_signaled = false;
            m_closed.store(false, std::memory_order_release);
        }

        /**
         * @brief Report whether the state is closed.
         * @return Snapshot of the closed flag.
         */
        [[nodiscard]] auto IsClosed() const noexcept -> bool {
            return m_closed.load(std::memory_order_acquire);
        }

        /**
         * @brief Report whether a notification is latched.
         * @return Snapshot of the signaled flag.
         */
        [[nodiscard]] auto IsSignaled() const -> bool {
            std::lock_guard lock{ m_mutex };
            return m_signaled;
        }

        /**
         * @brief Wait until notified, closed, cancelled, or past an optional deadline.
         * @param self State kept alive for the whole wait.
         * @param deadline Absolute timeout; std::nullopt waits indefinitely.
         * @return Reason the wait resumed.
         */
        static auto WaitUntil(
            std::shared_ptr<State> self,
            std::optional<std::chrono::steady_clock::time_point> deadline
        ) -> net::awaitable<WaitResult> {
            auto waiter{ std::make_shared<Waiter>(self->m_executor) };
            {
                std::lock_guard lock{ self->m_mutex };
                if (self->m_closed.load(std::memory_order_acquire)) {
                    co_return WaitResult::Closed;
                }
                if (self->m_signaled) {
                    self->m_signaled = false;
                    co_return WaitResult::Notified;
                }
                waiter->pending = true;
                self->m_waiters.push_back(waiter);
            }

            // On expiry the timer dequeues the waiter and closes its channel, resuming the
            // receive below with result == TimedOut.
            std::optional<net::steady_timer> timer;
            if (deadline) {
                timer.emplace(self->m_executor);
                timer->expires_at(*deadline);
                timer->async_wait([self, waiter](boost::system::error_code timer_ec) {
                    if (!timer_ec) {
                        self->abortWaiter(waiter, WaitResult::TimedOut);
                    }
                });
            }

            boost::system::error_code ec;
            co_await waiter->channel.async_receive(net::redirect_error(net::use_awaitable, ec));
            if (timer) {
                timer->cancel();
            }

            std::lock_guard lock{ self->m_mutex };
            if (!waiter->pending) {
                co_return waiter->result;
            }
            // Still queued: the event did not complete this waiter, so cancellation resumed it.
            self->removeLocked(waiter.get());
            waiter->pending = false;
            if (self->m_closed.load(std::memory_order_acquire)) {
                co_return WaitResult::Closed;
            }
            if (ec && !isCancellationError(ec)) {
                co_return WaitResult::Failed;
            }
            co_return WaitResult::Cancelled;
        }

    private:
        /**
         * @brief Dequeue every waiter and mark it completed with a result.
         * @param result Outcome given to each waiter.
         * @return Dequeued waiters whose channels the caller must close outside the lock.
         */
        auto takeAllLocked(WaitResult result) -> std::vector<std::shared_ptr<Waiter>> {
            std::vector<std::shared_ptr<Waiter>> targets;
            targets.reserve(m_waiters.size());
            for (auto& waiter : m_waiters) {
                waiter->pending = false;
                waiter->result = result;
                targets.push_back(std::move(waiter));
            }
            m_waiters.clear();
            return targets;
        }

        /**
         * @brief Remove a waiter from the queue; the caller holds m_mutex.
         * @param waiter Waiter to remove.
         */
        auto removeLocked(Waiter* waiter) -> void {
            std::erase_if(m_waiters, [waiter](auto const& queued) { return queued.get() == waiter; });
        }

        /**
         * @brief Complete a still-pending waiter from outside the event, e.g. on timeout.
         * @param waiter Waiter to complete.
         * @param result Outcome given to the waiter.
         */
        auto abortWaiter(std::shared_ptr<Waiter> const& waiter, WaitResult result) -> void {
            {
                std::lock_guard lock{ m_mutex };
                if (!waiter->pending) {
                    return;
                }
                removeLocked(waiter.get());
                waiter->pending = false;
                waiter->result = result;
            }
            waiter->channel.close();
        }

        /**
         * @brief Classify errors that a cancelled channel receive reports.
         * @param ec Error from async_receive.
         * @return True for cancellation or channel-close errors.
         */
        static auto isCancellationError(boost::system::error_code ec) -> bool {
            return ec == net::error::operation_aborted || ec == net::experimental::error::channel_cancelled || ec == net::experimental::error::channel_closed;
        }
    };

    AsyncEvent::AsyncEvent(net::any_io_executor ex)
        : m_state{ std::make_shared<State>(std::move(ex)) } {}

    AsyncEvent::~AsyncEvent() {
        // Each in-flight wait holds its own reference to m_state, so closing never races
        // with a waiter's state.
        m_state->Close();
    }

    auto AsyncEvent::GetExecutor() const -> net::any_io_executor {
        return m_state->GetExecutor();
    }

    auto AsyncEvent::NotifyOne() -> NotifyResult {
        return m_state->NotifyOne();
    }

    auto AsyncEvent::NotifyAll() -> NotifyResult {
        return m_state->NotifyAll();
    }

    auto AsyncEvent::Wait() -> net::awaitable<WaitResult> {
        return State::WaitUntil(m_state, std::nullopt);
    }

    auto AsyncEvent::WaitFor(
        std::chrono::steady_clock::duration timeout
    ) -> net::awaitable<WaitResult> {
        return State::WaitUntil(m_state, std::chrono::steady_clock::now() + timeout);
    }

    auto AsyncEvent::TryWait() -> bool {
        return m_state->TryWait();
    }

    auto AsyncEvent::Close() -> void {
        m_state->Close();
    }

    auto AsyncEvent::Reset() -> void {
        m_state->Reset();
    }

    auto AsyncEvent::IsClosed() const noexcept -> bool {
        return m_state->IsClosed();
    }

    auto AsyncEvent::IsSignaled() const -> bool {
        return m_state->IsSignaled();
    }

} // namespace mhttp::util
