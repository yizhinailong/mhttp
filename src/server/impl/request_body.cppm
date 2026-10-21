/**
 * @file request_body.cppm
 * @brief Decode bounded owned request chunks while retaining parser and transport state through lazy Tasks.
 */
module mhttp:server.impl.request_body;

import std;
import asio;
import :core.impl.bridge;
import :server.impl.request_input;

namespace mhttp::impl {

    /**
     * @brief Own one lazy request parser whose active window ends before response finalization.
     * @note Mutable state uses the connection strand. Issued Tasks retain this object, never the
     *       borrowed Request or Session; retirement prevents them from consuming a later request.
     */
    class RequestBodyImpl : public std::enable_shared_from_this<RequestBodyImpl> {
    private:
        using Clock = std::chrono::steady_clock; ///< Monotonic body intake deadline clock.
        std::shared_ptr<Connection> m_connection; ///< Executor and owned transport for issued reads.
        std::shared_ptr<RequestInput> m_input; ///< Connection input, including unconsumed pipeline bytes.
        detail::RequestParser m_parser; ///< This request's parser, moved after header admission.
        Clock::time_point m_deadline; ///< Absolute deadline checked by every body read.
        std::chrono::milliseconds m_write_timeout; ///< Bound for a deferred 100 Continue write.
        bool m_expect_continue{}; ///< Interim response deferred until the first accepted read.
        std::size_t m_chunk_bytes; ///< Positive maximum input and decoded chunk size.
        std::function<void(Headers)> m_on_complete; ///< Weak request callback, invoked only on this strand.
        std::optional<detail::ProtocolFailure> m_failure; ///< First failure, even if ignored by the handler.
        bool m_complete{}; ///< Empty framing or a consumed Complete event.
        bool m_busy{}; ///< Exactly one active read may mutate the parser and shared input.
        bool m_retired{}; ///< No reads may touch this connection after the route chain drains.

        /**
         * @brief Retain only the first body failure for final error handling and mandatory connection closure.
         * @param failure Owned protocol/transport cause and response status.
         */
        auto fail(detail::ProtocolFailure failure) -> void {
            if (!m_failure) { m_failure = failure; }
        }

        /**
         * @brief Publish body completion and owned trailers on the connection strand.
         */
        auto complete() -> void {
            m_complete = true;
            if (m_on_complete) { m_on_complete(m_parser.Trailers()); }
        }

        /**
         * @brief Decode one owned chunk or end marker without exposing retained transport storage.
         * @param self State retained before initial suspension; accessed only on its connection strand.
         * @return Nonempty chunk, nullopt after completion, or a sticky failure.
         */
        static auto read(
            std::shared_ptr<RequestBodyImpl> self
        ) -> asio::awaitable<Result<std::optional<std::string>>> {
            if (self->m_retired) { co_return std::unexpected{ Error::InvalidRequest }; }
            if (self->m_failure) { co_return std::unexpected{ self->m_failure->error }; }
            if (self->m_busy) {
                self->fail({ Error::Busy, Status::InternalServerError });
                co_return std::unexpected{ Error::Busy };
            }
            if (self->m_complete) { co_return std::optional<std::string>{}; }
            if (self->m_parser.Feed({}).event == detail::ParseEvent::Complete) {
                self->complete();
                co_return std::optional<std::string>{};
            }
            if (Clock::now() >= self->m_deadline) {
                self->fail({ Error::ReadTimeout, Status::RequestTimeout });
                co_return std::unexpected{ Error::ReadTimeout };
            }
            self->m_busy = true;
            /**
             * @brief Release the read guard after completion, exceptions, and cancellation.
             */
            struct BusyLease {
                bool& active; ///< Guard in the owned reader state.
                /**
                 * @brief Release before any later read or session continuation resumes.
                 */
                ~BusyLease() { active = false; }
            } lease{ self->m_busy };
            if (self->m_expect_continue) {
                self->m_expect_continue = false;
                auto interim = co_await self->m_connection->WriteAll("HTTP/1.1 100 Continue\r\n\r\n",
                    std::min<Clock::duration>(self->m_write_timeout, self->m_deadline - Clock::now()));
                if (!interim) {
                    self->fail({ interim.error(), Status::InternalServerError });
                    co_return std::unexpected{ interim.error() };
                }
            }
            auto event = co_await self->m_input->ReadEvent(self->m_parser, self->m_deadline, self->m_chunk_bytes);
            if (!event) {
                if (event.error() == Error::ConnectionClosed) { event = self->m_parser.Finish(); }
                else {
                    self->fail({ event.error(), event.error() == Error::ReadTimeout
                        ? Status::RequestTimeout : Status::BadRequest });
                    co_return std::unexpected{ event.error() };
                }
            }
            if (self->m_failure) { co_return std::unexpected{ self->m_failure->error }; }
            if (event->event == detail::ParseEvent::Failed) {
                self->fail(*event->failure);
                co_return std::unexpected{ self->m_failure->error };
            }
            if (event->event == detail::ParseEvent::Body) {
                co_return std::optional<std::string>{ std::string{ event->data.data(), event->data.size() } };
            }
            self->complete();
            co_return std::optional<std::string>{};
        }

    public:
        /**
         * @brief Own admitted framing and the shared input without retaining the public request.
         * @param connection Owned initialized transport, accessed on its strand.
         * @param input Owned connection input, currently paused immediately after the request head.
         * @param parser Owned admitted parser with its original total body/header limits.
         * @param deadline Absolute body phase deadline; application work does not reset it.
         * @param chunk_bytes Positive bound on each returned decoded chunk.
         * @param write_timeout Maximum duration for the optional deferred interim response.
         * @param complete Whether framing declares an empty body, allowing persistence without a read.
         */
        RequestBodyImpl(
            std::shared_ptr<Connection> connection,
            std::shared_ptr<RequestInput> input,
            detail::RequestParser parser,
            Clock::time_point deadline,
            std::size_t chunk_bytes,
            std::chrono::milliseconds write_timeout,
            bool complete
        ) : m_connection{ std::move(connection) }, m_input{ std::move(input) },
            m_parser{ std::move(parser) }, m_deadline{ deadline }, m_write_timeout{ write_timeout },
            m_expect_continue{ m_parser.Framing().expect_continue && !complete }, m_chunk_bytes{ chunk_bytes },
            m_complete{ complete } {}

        /**
         * @brief Set the weak request-storage callback before invoking any application middleware.
         * @param callback Owned callback receiving validated trailers only after complete decoding.
         */
        auto OnComplete(std::function<void(Headers)> callback) -> void { m_on_complete = std::move(callback); }

        /**
         * @brief Retain state before returning a lazy Task, dispatching all parser mutation to the connection strand.
         * @return Owned chunk/end/error operation; sequential reads must each be awaited.
         */
        [[nodiscard]] auto Read() -> Task<Result<std::optional<std::string>>> {
            return ToTask(m_connection->Executor(), asio::co_spawn(m_connection->Executor(),
                read(shared_from_this()), asio::use_awaitable));
        }

        /**
         * @brief Inspect complete body consumption on the connection strand.
         * @return Whether it is safe to advance to a successor request when no failure exists.
         */
        [[nodiscard]] auto Complete() const noexcept -> bool { return m_complete; }

        /**
         * @brief Inspect the first body failure before final exception/error/post processing.
         * @return Owned failure, or nullopt when intake has not failed.
         */
        [[nodiscard]] auto Failure() const noexcept -> std::optional<detail::ProtocolFailure> { return m_failure; }

        /**
         * @brief End the route's reading window before any response write or connection reuse.
         * @note Call only on the connection strand after nested route Tasks have drained.
         */
        auto Retire() -> void {
            if (m_busy) { fail({ Error::Busy, Status::InternalServerError }); m_connection->Close(); }
            m_retired = true;
            m_on_complete = {};
        }
    };

    /**
     * @brief Retire lazy intake even when cancellation prevents router processing from starting.
     */
    struct RequestBodyRetirement {
        std::shared_ptr<RequestBodyImpl> state; ///< Optional reader retained through session cleanup.
        /**
         * @brief Prevent escaped operations from consuming a successor on every exit path.
         */
        ~RequestBodyRetirement() { if (state) { state->Retire(); } }
    };

} // namespace mhttp::impl
