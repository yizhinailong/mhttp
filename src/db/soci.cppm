/**
 * @file soci.cppm
 * @brief Run blocking SOCI database work with pooled sessions on Runtime blocking-work threads.
 */
module;

#include <soci/soci.h>
#include <soci/sqlite3/soci-sqlite3.h>

export module mhttp.soci;

import std;
export import mhttp;

export namespace soci {

    using soci::backend_factory;
    using soci::blob;
    using soci::column_properties;
    using soci::connection_parameters;
    using soci::i_null;
    using soci::i_ok;
    using soci::i_truncated;
    using soci::indicator;
    using soci::into;
    using soci::row;
    using soci::rowid;
    using soci::rowset;
    using soci::session;
    using soci::soci_error;
    using soci::sqlite3;
    using soci::sqlite3_soci_error;
    using soci::statement;
    using soci::transaction;
    using soci::type_conversion;
    using soci::use;
    using soci::values;

} // namespace soci

export namespace mhttp::db {

    /**
     * @brief Configure the sessions connected eagerly by SessionPool::Open.
     * @note Both values must be positive; acquire_timeout must also fit SOCI's int millisecond wait.
     */
    struct SessionPoolOptions {
        std::size_t size{ 2 }; ///< Number of independently connected sessions.
        std::chrono::milliseconds acquire_timeout{ 5'000 }; ///< Worker-thread wait for an idle session.
    };

} // namespace mhttp::db

namespace mhttp::detail {

    /**
     * @brief Share connected sessions with every SessionPool copy and queued or running Execute work.
     */
    struct SessionPoolState {
        soci::connection_pool pool; ///< Thread-safe lease bookkeeping and session storage.
        db::SessionPoolOptions options; ///< Validated immutable configuration.

        /**
         * @brief Allocate unconnected sessions.
         * @param configured Validated positive size and acquisition timeout.
         */
        explicit SessionPoolState(db::SessionPoolOptions configured) :
            pool{ configured.size }, options{ configured } {}
    };

    /**
     * @brief Lease one session exclusively and return it when blocking work finishes or throws.
     * @note Member definitions stay in this module, so its own object file carries the throw sites.
     */
    class SessionLease {
    private:
        SessionPoolState& m_state; ///< Pool retained by the enclosing Offload function.
        std::size_t m_position{}; ///< Exclusively leased session index.

        /**
         * @brief Reject a moved-from pool before waiting for a session.
         * @param state Pool shared with the issuing SessionPool, or null after a move.
         * @return The non-null pool.
         * @throws std::system_error With NotRunning for a null pool.
         */
        static auto require(SessionPoolState* state) -> SessionPoolState& {
            if (state == nullptr) { throw std::system_error{ make_error_code(Error::NotRunning) }; }
            return *state;
        }

    public:
        /**
         * @brief Wait on the current worker thread for an idle session.
         * @param state Pool kept alive by the caller for the lease lifetime, or null after a move.
         * @throws std::system_error With NotRunning for a null pool, or AcquireTimeout when every
         *         session stays leased for the configured timeout.
         */
        explicit SessionLease(SessionPoolState* state) : m_state{ require(state) } {
            auto const timeout = static_cast<int>(m_state.options.acquire_timeout.count());
            if (!m_state.pool.try_lease(m_position, timeout)) {
                throw std::system_error{ make_error_code(Error::AcquireTimeout) };
            }
        }

        /**
         * @brief Prevent returning one session twice.
         * @param other Unused source.
         */
        SessionLease(SessionLease const& other) = delete;

        /**
         * @brief Prevent returning one session twice.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(SessionLease const& other) -> SessionLease& = delete;

        /**
         * @brief Make the session available to the next waiting worker.
         */
        ~SessionLease() { m_state.pool.give_back(m_position); }

        /**
         * @brief Access the session exclusively owned by this lease.
         * @return Session valid until the lease is destroyed.
         */
        [[nodiscard]] auto Session() const -> soci::session& { return m_state.pool.at(m_position); }
    };

} // namespace mhttp::detail

export namespace mhttp::db {

    /**
     * @brief Own a fixed set of connected SOCI sessions and run blocking statements away from I/O threads.
     * @note Each Execute function receives one session exclusively on an Offload worker, so concurrent
     *       work is bounded by the pool size and by the awaiting Runtime's blocking-work threads.
     *       Copies share the same sessions, which close when the last copy and every issued Task are gone.
     *       Members are thread-safe; assigning or destroying one object still needs external synchronization.
     */
    class SessionPool {
    private:
        std::shared_ptr<detail::SessionPoolState> m_state; ///< Sessions retained by issued Execute Tasks.

        /**
         * @brief Adopt sessions that are already connected.
         * @param state Owned pool state.
         */
        explicit SessionPool(std::shared_ptr<detail::SessionPoolState> state) : m_state{ std::move(state) } {}

    public:
        /**
         * @brief Connect every session before any statement can run.
         * @param backend Factory such as soci::sqlite3; it must outlive the pool, its copies, and issued Tasks.
         * @param connect_string Backend-specific connection string, copied by each session.
         * @param options Positive pool size and acquisition timeout.
         * @return Connected pool, InvalidRequest for invalid options, or Busy on any Runtime I/O thread.
         * @throws soci::soci_error When the backend cannot open a session; already opened sessions are closed.
         * @note Connecting blocks the calling thread. Open the pool before serving, or inside Offload.
         */
        [[nodiscard]] static auto Open(
            soci::backend_factory const& backend,
            std::string const& connect_string,
            SessionPoolOptions options = {}
        ) -> Result<SessionPool> {
            if (options.size == 0 || options.acquire_timeout <= std::chrono::milliseconds::zero()
                || options.acquire_timeout.count() > std::numeric_limits<int>::max()) {
                return std::unexpected{ Error::InvalidRequest };
            }
            if (Runtime::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            auto state = std::make_shared<detail::SessionPoolState>(options);
            for (std::size_t index{}; index < options.size; ++index) {
                state->pool.at(index).open(backend, connect_string);
            }
            return SessionPool{ std::move(state) };
        }

        /**
         * @brief Report the configured session count.
         * @return Number of sessions, or zero for a moved-from pool.
         */
        [[nodiscard]] auto Size() const noexcept -> std::size_t {
            return m_state ? m_state->options.size : 0;
        }

        /**
         * @brief Run blocking database work with one exclusive session on a blocking-work thread.
         * @tparam Function Owned invocable accepting soci::session&; move-only callables are supported.
         * @param function Function moved into the lazy Task and invoked at most once.
         * @return Owned function result, or void. References are not supported.
         * @throws std::system_error With AcquireTimeout when no session frees up in time, NotRunning for a
         *         moved-from pool, or Canceled as reported by Offload. Function exceptions, including
         *         soci::soci_error, propagate unchanged after the session is returned to the pool.
         * @note The Task retains the pool, so it may outlive this object. The session is valid only during
         *       the call; do not retain it or statements bound to it. Use soci::transaction for multi-statement
         *       work so an exception rolls back before the session is reused. Anything else the function
         *       borrows must outlive the returned Task.
         */
        template <typename Function>
            requires std::invocable<Function&, soci::session&>
        [[nodiscard]] auto Execute(
            Function function
        ) const -> Task<std::invoke_result_t<Function&, soci::session&>> {
            using Value = std::invoke_result_t<Function&, soci::session&>;
            static_assert(!std::is_reference_v<Value>, "Execute must return an owned value or void");
            return Offload([state = m_state, function = std::move(function)] mutable -> Value {
                detail::SessionLease lease{ state.get() };
                return std::invoke(function, lease.Session());
            });
        }
    };

} // namespace mhttp::db
