/**
 * @file transport_tests.cppm
 * @brief Exercise private transport behavior through a fixture exported only in a temporary project.
 */
export module mhttp:test.transport;

import std;
import asio;
import :core.error;
import :core.impl.io_error;

#include "test_support.hpp"

namespace mhttp::test {

    /**
     * @brief Verify success, cancellation, deadline precedence, peer closure, and operation fallbacks.
     */
    auto io_error_mapping() -> void {
        using enum impl::IoOperation;
        for (auto operation : { Connect, Resolve, Bind, Read, Write }) {
            Check(!impl::map_io_error({}, operation), "successful completion has no error");
            CheckEq(impl::map_io_error(asio::error::operation_aborted, operation),
                std::optional{ Error::Canceled }, "external cancellation stays distinct from timeouts");
        }
        struct Case {
            asio::error_code error; ///< Backend outcome.
            impl::IoOperation operation; ///< Operation being performed.
            bool expired{}; ///< A winning local deadline.
            Error expected; ///< Stable public error.
        };
        Case const cases[]{
            { asio::error::timed_out, Connect, false, Error::ConnectionTimeout },
            { asio::error::timed_out, Read, false, Error::ReadTimeout },
            { asio::error::timed_out, Write, false, Error::WriteTimeout },
            { asio::error::operation_aborted, Read, true, Error::ReadTimeout },
            { asio::error::operation_aborted, Write, true, Error::WriteTimeout },
            { {}, Connect, true, Error::ConnectionTimeout },
            { asio::error::eof, Read, false, Error::ConnectionClosed },
            { asio::error::connection_reset, Read, false, Error::ConnectionClosed },
            { asio::error::connection_aborted, Read, false, Error::ConnectionClosed },
            { asio::error::broken_pipe, Write, false, Error::ConnectionClosed },
            { asio::error::not_connected, Write, false, Error::ConnectionClosed },
            { asio::error::shut_down, Write, false, Error::ConnectionClosed },
            { asio::error::connection_refused, Connect, false, Error::Connection },
            { asio::error::connection_reset, Connect, false, Error::Connection },
            { asio::error::host_not_found, Resolve, false, Error::ResolveFailed },
            { asio::error::timed_out, Resolve, false, Error::ResolveFailed },
            { asio::error::address_in_use, Bind, false, Error::BindIpAddress },
            { asio::error::timed_out, Bind, false, Error::BindIpAddress },
            { asio::error::fault, Read, false, Error::Read },
            { asio::error::fault, Write, false, Error::Write },
            { asio::error::fault, static_cast<impl::IoOperation>(999), false, Error::Unknown },
        };
        for (auto const& test : cases) {
            CheckEq(impl::map_io_error(test.error, test.operation, test.expired),
                std::optional{ test.expected }, "contextual transport error mapping");
        }
    }

} // namespace mhttp::test

export namespace mhttp::test {

    /**
     * @brief Run the private transport fixture without exporting it from the production library.
     * @param argc Argument count.
     * @param argv Case name filters.
     * @return Test runner exit status.
     */
    auto RunTransportTests(int argc, char* argv[]) -> int {
        return RunTests(argc, argv, {
            { "I/O error mapping", io_error_mapping },
        });
    }

} // namespace mhttp::test
