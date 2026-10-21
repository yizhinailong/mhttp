/**
 * @file asio_import_test.cpp
 * @brief Verify that network test programs can explicitly import the Asio dependency.
 */
import std;
import mhttp;
import asio;

#include "support/test_support.hpp"

/**
 * @brief Construct an independent Asio context without opening any sockets.
 */
auto asio_is_available() -> void {
    asio::io_context context;
    mhttp::test::Check(!context.stopped(), "explicitly imported Asio can construct a context");
}

/**
 * @brief Run the dependency import probe.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, { { "explicit Asio import", asio_is_available } });
}
