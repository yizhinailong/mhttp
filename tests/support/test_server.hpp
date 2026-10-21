/**
 * @file test_server.hpp
 * @brief Own a public loopback listener and runtime with checked teardown for standalone tests.
 * @note Include after importing std and mhttp and including test_support.hpp. Use on non-I/O threads.
 */
#pragma once

namespace mhttp::test {

    /**
     * @brief Register routes through Server(), then start a dynamic IPv4 loopback listener.
     * @note Destruction joins all server callbacks before destroying the borrowed runtime.
     *       Clients and issued operations using Execution() must finish before this owner is destroyed.
     */
    class TestServer {
    private:
        mhttp::Runtime m_runtime; ///< Declared first to outlive its listener.
        server::HttpServer m_server; ///< Stopped before runtime shutdown, including active Tasks.

        /**
         * @brief Throw on setup failure so the test runner can report it and continue safely.
         * @param result Completed setup outcome.
         */
        static auto requireSuccess(Result<void> result) -> void {
            if (!result) { throw std::runtime_error{ std::string{ ToString(result.error()) } }; }
        }

    public:
        /**
         * @brief Start I/O execution while leaving route registration open until Start().
         * @param threads Positive I/O execution thread count.
         * @param options Owned listener limits and deadlines.
         */
        explicit TestServer(std::size_t threads = 2, server::ServerOptions options = {}) :
            m_server{ m_runtime, std::move(options) } { requireSuccess(m_runtime.Start(threads)); }

        /**
         * @brief Cancel and join sessions before checking listener and runtime cleanup.
         */
        ~TestServer() {
            Check(m_server.Stop().has_value(), "test listener drains on destruction");
            Check(!m_server.IsRunning() && !m_server.LocalEndpoint(), "test listener retains no active generation");
            m_runtime.Stop();
            Check(m_runtime.Join().has_value(), "test runtime drains after its listener");
        }

        /**
         * @brief Borrow the listener for route registration, hooks, and lifecycle assertions.
         * @return Listener retained until this fixture is destroyed.
         */
        auto Server() -> server::HttpServer& { return m_server; }

        /**
         * @brief Borrow execution for clients and SyncWait calls scoped within this fixture.
         * @return Longer-lived runtime for the contained listener.
         */
        auto Execution() -> mhttp::Runtime& { return m_runtime; }

        /**
         * @brief Bind only to a dynamic loopback port, submit acceptance, and wait for initialization.
         * @return Actual port, with failures reported as exceptions.
         */
        auto Start() -> std::uint16_t {
            auto port = m_server.BindToAnyPort("127.0.0.1");
            if (!port) { throw std::runtime_error{ std::string{ ToString(port.error()) } }; }
            requireSuccess(m_server.Start());
            requireSuccess(m_server.WaitUntilReady());
            return *port;
        }
    };

} // namespace mhttp::test
