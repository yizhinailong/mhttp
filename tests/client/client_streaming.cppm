/**
 * @file client_streaming.cppm
 * @brief Verify streamed raw response framing, malicious input, and callback deadlines.
 */
module mhttp:test.client_streaming;

import std;
import :client.http_client;
import :test.client_peer;

namespace mhttp::test {

    /**
     * @brief Decode raw fixed, chunked, EOF, HEAD, 204, and 304 responses using one-byte bounded receivers.
     */
    auto client_streamed_raw_framing() -> void {
        auto const binary = std::string("x\0y", 3);
        std::array<std::string, 6> wire{
            "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n" + binary,
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-End\r\n\r\n3\r\n" + binary + "\r\n0\r\nX-End: yes\r\n\r\n",
            "HTTP/1.0 200 OK\r\n\r\n" + binary,
            "HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\n",
            "HTTP/1.1 204 No Content\r\n\r\n",
            "HTTP/1.1 304 Not Modified\r\nContent-Length: 99\r\n\r\n",
        };
        for (std::size_t index{}; index != wire.size(); ++index) {
            std::string body;
            unsigned heads{}, complete{};
            std::optional<std::uint64_t> total;
            std::uint64_t downloaded{};
            client::TransferCallbacks callbacks;
            callbacks.on_headers = [&](auto const& head) -> Task<Result<void>> {
                ++heads;
                Check(head.Body().empty() && head.Trailers().Empty() && !head.KeepAlive(), "raw header snapshot is incomplete immutable metadata");
                co_return Result<void>{};
            };
            callbacks.on_body = [&](std::string bytes) -> Task<Result<void>> {
                CheckEq(bytes.size(), std::size_t{ 1 }, "raw response receiver obeys one-byte cap");
                body += bytes;
                co_return Result<void>{};
            };
            callbacks.on_progress = [&](auto sample) -> Task<Result<void>> {
                if (sample.direction == client::TransferDirection::Download) {
                    Check(sample.transferred >= downloaded, "raw decoded progress is monotonic");
                    downloaded = sample.transferred;
                    total = sample.total;
                    if (sample.complete) { ++complete; }
                }
                co_return Result<void>{};
            };
            auto result = run_client_peer({ wire[index] }, { .response_chunk_bytes = 1 },
                client::Request{ index == 3 ? Method::Head : Method::Get, "/" }, std::move(callbacks));
            Check(result.response && result.response->Body().empty() && body == (index < 3 ? binary : ""), "streamed raw framing preserves binary and suppresses forbidden bodies");
            Check(heads == 1 && complete == 1 && downloaded == body.size(), "one final raw head and one complete download");
            CheckEq(total, index == 1 || index == 2 ? std::nullopt : std::optional<std::uint64_t>{ index < 3 ? 3u : 0u },
                "only fixed decoded body lengths have a declared total");
            if (index == 1) { CheckEq(result.response->Trailers().Get("X-End"), "yes", "streamed raw trailers survive completion"); }
            if (index == 2) { Check(!result.response->KeepAlive() && !result.open, "EOF stream retires its connection"); }
        }
    }

    /**
     * @brief Reject malformed or truncated streamed input without reporting successful completion of a partial sink.
     */
    auto client_streamed_raw_failures() -> void {
        /**
         * @brief Own a raw failure script and the only decoded prefix allowed to reach the receiver.
         */
        struct Case {
            std::string wire; ///< Raw response, deliberately invalid or truncated.
            Error error; ///< Expected first parser/transport cause.
            std::string prefix; ///< Successfully accepted decoded bytes before failure.
        };
        std::array cases{
            Case{ "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nab", Error::ConnectionClosed, "ab" },
            Case{ "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n", Error::ConnectionClosed, "ab" },
            Case{ "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n", Error::BodyTooLarge, "" },
            Case{ "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n2\r\ncd\r\n0\r\n\r\n", Error::BodyTooLarge, "ab" },
            Case{ "HTTP/1.0 200 OK\r\n\r\nabcd", Error::BodyTooLarge, "abc" },
            Case{ "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nx\r\n", Error::ProtocolError, "" },
        };
        for (auto const& test : cases) {
            std::string body;
            unsigned complete{};
            client::TransferCallbacks callbacks;
            callbacks.on_body = [&](std::string bytes) -> Task<Result<void>> { body += bytes; co_return Result<void>{}; };
            callbacks.on_progress = [&](auto sample) -> Task<Result<void>> {
                if (sample.direction == client::TransferDirection::Download && sample.complete) { ++complete; }
                co_return Result<void>{};
            };
            auto result = run_client_peer({ test.wire }, { .body_bytes = 3, .response_chunk_bytes = 1 }, client::Request{}, std::move(callbacks));
            Check(!result.response && result.response.error() == test.error && !result.open, "streaming retains protocol/size/EOF validation and closes");
            CheckEq(body, test.prefix, "only validated body prefix reaches streaming sink");
            CheckEq(complete, 0u, "failed streamed input never reports completion");
        }
        unsigned delivered{};
        client::TransferCallbacks callbacks;
        callbacks.on_headers = [](auto const&) -> Task<Result<void>> { co_return std::unexpected{ Error::InvalidRequest }; };
        callbacks.on_body = [&](std::string) -> Task<Result<void>> { ++delivered; co_return Result<void>{}; };
        auto rejected = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc" }, {}, client::Request{}, std::move(callbacks));
        Check(!rejected.response && rejected.response.error() == Error::InvalidRequest && delivered == 0 && !rejected.open,
            "header rejection prevents body delivery even when response bytes arrived together");
    }

    /**
     * @brief Apply all deadline policies to awaited download consumers, including already buffered input.
     */
    auto client_streamed_callback_policies() -> void {
        for (auto policy : { client::TimeoutPolicy::Phase, client::TimeoutPolicy::Overall,
                 client::TimeoutPolicy::Step, client::TimeoutPolicy::Never }) {
            std::string body;
            client::TransferCallbacks callbacks;
            callbacks.on_body = [&](std::string bytes) -> Task<Result<void>> {
                co_await Sleep(60ms);
                body += bytes;
                co_return Result<void>{};
            };
            auto result = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc" },
                { .read_timeout = 100ms, .timeout_policy = policy, .timeout = 100ms, .response_chunk_bytes = 1 },
                client::Request{}, std::move(callbacks));
            if (policy == client::TimeoutPolicy::Step || policy == client::TimeoutPolicy::Never) {
                Check(result.response && body == "abc", "Step renews receiver budgets; Never permits awaited consumers");
            } else { Check(!result.response && result.response.error() == Error::ReadTimeout, "Phase/Overall bound complete response consumption"); }
        }
    }


} // namespace mhttp::test
