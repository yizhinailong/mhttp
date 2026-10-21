/**
 * @file json_support_test.cpp
 * @brief Verify that importing the core library never requires or implicitly activates its optional JSON adapter.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

/**
 * @brief Detect only an available JSON reader, without making unsupported access a hard compile error.
 * @tparam T Message type.
 */
template <typename T>
concept HasJsonReader = requires(T const& value) { value.Json(); };

/**
 * @brief Detect JSON serialization of a candidate type.
 * @tparam Message Mutable message type.
 * @tparam T Candidate body type.
 */
template <typename Message, typename T>
concept HasJsonWriter = requires(Message& message, T const& value) { message.SetJson(value); };

/**
 * @brief Keep JSON implementation and overloads absent from an import-mhttp-only consumer.
 */
auto core_json_isolation() -> void {
    static_assert(!HasJsonReader<mhttp::server::Request>);
    static_assert(!HasJsonReader<mhttp::client::Response>);
    static_assert(!HasJsonWriter<mhttp::server::Response, std::string>);
    static_assert(!HasJsonWriter<mhttp::client::Request, std::string>);
    mhttp::test::CheckEq(mhttp::JsonLimits{}.max_depth, std::size_t{ 128 }, "core configuration remains available without JSON types");
}

/**
 * @brief Run compile-time dependency isolation evidence as a normal core test program.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, { { "core JSON dependency isolation", core_json_isolation } });
}
