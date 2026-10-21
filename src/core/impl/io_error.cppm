/**
 * @file io_error.cppm
 * @brief Translate transport failures consistently without exposing Asio error categories.
 */
module mhttp:core.impl.io_error;

import std;
import asio;
import :core.error;

namespace mhttp::impl {

    /**
     * @brief Throw an owned mhttp error through standard exception handling.
     * @param error Library failure to report at a coroutine or synchronous wait boundary.
     * @note Keep this definition before imported I/O/thread/filesystem code is instantiated: on Clang/MSVC,
     *       otherwise system_error throw metadata can omit its copy constructor and double-free
     *       the owned message during exception transport. Native I/O implementations import this partition.
     */
    [[noreturn]] auto throw_error(Error error) -> void {
        throw std::system_error{ make_error_code(error) };
    }

    /**
     * @brief Identify the operation whose fallback and timeout errors should be reported.
     */
    enum class IoOperation {
        Connect, ///< TCP connection establishment.
        Resolve, ///< Hostname resolution.
        Bind, ///< Local address binding or listening.
        Read, ///< Incoming bytes.
        Write, ///< Outgoing bytes.
    };

    /**
     * @brief Map a transport outcome, preserving success and distinguishing cancellation from timeout.
     * @param error Transport completion code.
     * @param operation Operation selecting fallback and timeout errors.
     * @param deadline_expired Whether this operation's own deadline caused cancellation.
     * @return No error on success; otherwise the corresponding public error.
     * @note Only a deadline that actually won the operation may set deadline_expired. Peer EOF/reset
     *       are ConnectionClosed for byte transfers; connect/resolve/bind keep their own context.
     */
    [[nodiscard]] auto map_io_error(
        asio::error_code error,
        IoOperation operation,
        bool deadline_expired = false
    ) noexcept -> std::optional<Error> {
        if (!error && !deadline_expired) { return std::nullopt; }
        if (deadline_expired || error == asio::error::timed_out) {
            switch (operation) {
            case IoOperation::Connect: return Error::ConnectionTimeout;
            case IoOperation::Read: return Error::ReadTimeout;
            case IoOperation::Write: return Error::WriteTimeout;
            case IoOperation::Resolve: return Error::ResolveFailed;
            case IoOperation::Bind: return Error::BindIpAddress;
            }
            return Error::Unknown;
        }
        if (error == asio::error::operation_aborted) { return Error::Canceled; }
        if (operation == IoOperation::Read || operation == IoOperation::Write) {
            if (error == asio::error::eof || error == asio::error::connection_reset
                || error == asio::error::connection_aborted || error == asio::error::broken_pipe
                || error == asio::error::not_connected || error == asio::error::shut_down) {
                return Error::ConnectionClosed;
            }
        }
        switch (operation) {
        case IoOperation::Connect: return Error::Connection;
        case IoOperation::Resolve: return Error::ResolveFailed;
        case IoOperation::Bind: return Error::BindIpAddress;
        case IoOperation::Read: return Error::Read;
        case IoOperation::Write: return Error::Write;
        }
        return Error::Unknown;
    }

} // namespace mhttp::impl
