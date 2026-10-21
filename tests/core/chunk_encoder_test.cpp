/**
 * @file chunk_encoder_test.cpp
 * @brief Verify chunk encoding through binary round trips, empty writes, and trailer validation.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"
#include "../support/parser_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using mhttp::test::check_all_splits;
using namespace mhttp::detail;
using namespace std::string_view_literals;

/**
 * @brief Verify empty writes never prematurely terminate a binary body and Finish emits once.
 */
auto chunks_and_completion() -> void {
    ChunkEncoder encoder;
    std::string body;
    for (auto chunk : { ""sv, "a\0b"sv, ""sv, "xyz"sv, ""sv }) {
        auto const result = encoder.Encode(chunk);
        if (Check(result.has_value(), "open encoder accepts binary and empty chunks")) { body += *result; }
        if (chunk.empty()) { Check(result && result->empty(), "zero-byte write emits no terminal chunk"); }
    }
    CheckEq(body, "3\r\na\0b\r\n3\r\nxyz\r\n"sv, "size counts binary bytes and uses hexadecimal framing");
    auto const finish = encoder.Finish();
    if (!Check(finish.has_value(), "Finish emits terminal bytes")) { return; }
    CheckEq(*finish, "0\r\n\r\n"sv, "empty trailers have an exact terminator");
    for (auto chunk : { ""sv, "later"sv }) {
        Check(encoder.Encode(chunk).error() == mhttp::Error::InvalidRequest, "no writes after Finish");
    }
    Check(encoder.Finish().error() == mhttp::Error::InvalidRequest, "Finish cannot emit twice");
    auto const header = serialize_response_head({}, mhttp::Method::Get, { .chunked = true });
    if (!Check(header.has_value(), "chunked head serializes")) { return; }
    check_all_splits<ResponseParser>(header->bytes + body + *finish, [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "chunked output parses at every boundary");
        CheckEq(parsed.body, "a\0bxyz"sv, "empty writes do not hide later bytes");
    });
}

/**
 * @brief Verify trailing fields retain order and forbidden/connection-specific fields fail atomically.
 */
auto trailers_and_injection() -> void {
    mhttp::Headers initial;
    Check(initial.Add("Connection", "keep-alive, X-Private").has_value(), "nominate private field");
    ChunkEncoder encoder{ initial };
    for (auto name : { "Host", "content-length", "TRANSFER-ENCODING", "Authorization", "Content-Type",
                       "Set-Cookie", "X-Private" }) {
        mhttp::Headers forbidden;
        Check(forbidden.Add("X-Allowed", "first").has_value(), "add valid prefix before forbidden trailer");
        Check(forbidden.Add(name, "value").has_value(), "construct lexically valid forbidden trailer");
        auto const rejected = encoder.Finish(forbidden);
        Check(!rejected && rejected.error() == mhttp::Error::InvalidHeaders, "forbidden trailer emits no partial terminator");
    }
    mhttp::Headers trailers;
    Check(trailers.Add("X-Sum", "1").has_value(), "first trailer");
    Check(trailers.Add("X-Other", "2").has_value(), "second trailer");
    Check(trailers.Add("X-Sum", "3").has_value(), "third trailer");
    for (auto value : { "yes\r\nInjected: true"sv, "a\nb"sv, "a\0b"sv }) {
        auto const rejected = trailers.Add("X-Invalid", value);
        Check(!rejected && rejected.error() == mhttp::Error::InvalidHeaders, "trailer mutation rejects injection before Finish");
    }
    auto const part = encoder.Encode("x");
    Check(part && *part == "1\r\nx\r\n", "failed Finish leaves encoder usable");
    auto const finish = encoder.Finish(trailers);
    if (!Check(finish.has_value(), "allowed trailers finish successfully")) { return; }
    CheckEq(*finish, "0\r\nX-Sum: 1\r\nX-Other: 2\r\nX-Sum: 3\r\n\r\n"sv, "trailer order retained without injected fields");
    auto const header = serialize_response_head({}, mhttp::Method::Get, { .chunked = true });
    if (!Check(header && part, "head and body available")) { return; }
    check_all_splits<ResponseParser>(header->bytes + *part + *finish, [](auto const& parsed) {
        Check(parsed.complete && parsed.body == "x" && !parsed.failure, "encoded trailer response completes");
        CheckEq(parsed.trailers.Count("X-Sum"), std::size_t{ 2 }, "repeated trailers remain separate");
        Check(!parsed.head.headers.Has("X-Sum"), "trailers never enter initial headers");
    });
}

/**
 * @brief Verify varied segment sizes reproduce deterministic binary input in both parser directions.
 */
auto binary_round_trip() -> void {
    std::string input(4096, '\0');
    for (std::size_t i{}; i < input.size(); ++i) { input[i] = static_cast<char>(i % 256); }
    ChunkEncoder encoder;
    std::string chunks;
    for (std::size_t offset{}; offset < input.size();) {
        auto const count = std::min(std::size_t{ 1 } + offset % 127, input.size() - offset);
        auto const encoded = encoder.Encode(std::string_view{ input }.substr(offset, count));
        if (!Check(encoded.has_value(), "encode varied binary segment")) { return; }
        chunks += *encoded;
        offset += count;
    }
    auto const finish = encoder.Finish();
    if (!Check(finish.has_value(), "terminate varied chunks")) { return; }
    chunks += *finish;
    auto verify = [&input](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "binary chunk stream completes");
        CheckEq(parsed.body, input, "all byte values survive varied chunks");
    };
    auto const response = serialize_response_head({}, mhttp::Method::Get, { .chunked = true });
    if (!Check(response.has_value(), "serialize chunked response")) { return; }
    check_all_splits<ResponseParser>(response->bytes + chunks, verify);
    RequestHead request;
    request.method = "POST";
    Check(request.headers.Set("Host", "localhost").has_value(), "set request authority");
    auto const head = serialize_request_head(request, { .chunked = true });
    if (!Check(head.has_value(), "serialize chunked request")) { return; }
    check_all_splits<RequestParser>(head->bytes + chunks, verify);
}

/**
 * @brief Run chunk encoder cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "chunks and completion", chunks_and_completion },
        { "trailers and injection", trailers_and_injection },
        { "binary round trip", binary_round_trip },
    });
}
