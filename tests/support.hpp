/**
 * @file support.hpp
 * @brief Shared helpers for mhttp tests: a loopback test server and a synchronous Beast client.
 * @note Include after `import std; import mhttp;` and after the Boost headers it uses
 *       (asio io_context/ip/tcp/executor_work_guard, beast flat_buffer/http). It has no
 *       includes of its own, so it never re-includes standard headers after `import std`.
 */
#pragma once

namespace test_support {

    namespace net = boost::asio;
    namespace http = boost::beast::http;
    using tcp = net::ip::tcp;
    using HttpRequest = http::request<http::string_body>;
    using HttpResponse = http::response<http::string_body>;

    /**
     * @brief Count failed expectations of the running test binary.
     * @return Failure counter.
     */
    inline auto failures() -> int& {
        static int s_failures{ 0 };
        return s_failures;
    }

    /**
     * @brief Report a failed behavioral expectation.
     * @param condition Whether the expectation holds.
     * @param message Failure diagnostic.
     * @param location Caller location, filled in automatically.
     * @return Whether the expectation holds.
     */
    inline auto check(
        bool condition,
        std::string_view message,
        std::source_location location = std::source_location::current()
    ) -> bool {
        if (!condition) {
            ++failures();
            std::println("  FAILED {}:{}: {}", location.file_name(), location.line(), message);
            std::fflush(nullptr);
        }
        return condition;
    }

    /**
     * @brief Substring a test case name must contain to run; set from argv[1] by init().
     * @return Filter text; empty runs every case.
     */
    inline auto case_filter() -> std::string& {
        static std::string s_filter;
        return s_filter;
    }

    /**
     * @brief Read the optional case filter from the command line.
     * @param argc Argument count.
     * @param argv Arguments; argv[1] filters cases by substring.
     */
    inline auto init(int argc, char** argv) -> void {
        if (argc > 1) {
            case_filter() = argv[1];
        }
    }

    /**
     * @brief Run a named test case and catch escaping exceptions.
     * @param name Test case name.
     * @param test Callable to run.
     */
    inline auto run_case(std::string_view name, auto&& test) -> void {
        if (!name.contains(case_filter())) {
            return;
        }
        auto const before{ failures() };
        std::println("...    {}", name);
        std::fflush(nullptr);
        try {
            test();
        } catch (std::exception const& e) {
            check(false, std::format("unexpected exception: {}", e.what()));
        }
        std::println("{} {}", failures() == before ? "ok    " : "FAILED", name);
        std::fflush(nullptr);
    }

    /**
     * @brief Print the summary line and return the process exit code.
     * @param binary Test binary name.
     * @return Zero when every check passed.
     */
    inline auto finish(std::string_view binary) -> int {
        std::println("{}: {}", binary, failures() == 0 ? "ok" : std::format("{} check(s) FAILED", failures()));
        return failures() == 0 ? 0 : 1;
    }

    /**
     * @brief Server on an ephemeral loopback port, driven by one I/O thread.
     */
    struct TestServer {
        net::io_context ioc;                                                                  ///< Server execution context.
        net::executor_work_guard<net::io_context::executor_type> guard{ ioc.get_executor() }; ///< Keeps ioc running after a stop.
        mhttp::HttpServer server{ ioc };                                                      ///< Server under test.
        std::future<boost::system::error_code> done;                                          ///< Result of the run cycle.
        std::jthread worker;                                                                  ///< Thread running ioc; joined first.

        /**
         * @brief Configure, listen on 127.0.0.1:0, and start running.
         * @param setup Registers routes and settings before the server runs.
         */
        explicit TestServer(std::function<void(mhttp::HttpServer&)> const& setup = {}) {
            if (setup) {
                setup(server);
            }
            if (auto const listened{ server.Listen("127.0.0.1", 0) }; !listened) {
                throw std::runtime_error{ "listen failed: " + listened.error().message() };
            }
            done = server.Run();
            worker = std::jthread{ [this] { ioc.run(); } };
        }

        TestServer(TestServer const&) = delete;
        auto operator=(TestServer const&) -> TestServer& = delete;

        /**
         * @brief Stop the server if a test did not, then let the I/O thread finish.
         */
        ~TestServer() {
            server.Stop().get();
            ioc.stop();
        }

        [[nodiscard]] auto Endpoint() const -> tcp::endpoint {
            return server.LocalEndpoint();
        }
    };

    /**
     * @brief Build a request with Host, extra headers, and a prepared payload.
     * @param method Method.
     * @param target Request target.
     * @param body Body bytes.
     * @param headers Extra header lines.
     * @return Request ready to send.
     */
    inline auto make_request(
        http::verb method,
        std::string_view target,
        std::string body = {},
        std::vector<std::pair<std::string, std::string>> const& headers = {}
    ) -> HttpRequest {
        HttpRequest req{ method, target, 11 };
        req.set(http::field::host, "127.0.0.1");
        for (auto const& [name, value] : headers) {
            req.set(name, value);
        }
        req.body() = std::move(body);
        req.prepare_payload();
        return req;
    }

    /**
     * @brief Synchronous HTTP client on one connection.
     */
    struct Client {
        net::io_context ioc;              ///< Context for synchronous socket operations.
        tcp::socket socket{ ioc };        ///< Connection to the server.
        boost::beast::flat_buffer buffer; ///< Read buffer kept across responses.

        /**
         * @brief Connect to an endpoint.
         * @param endpoint Server endpoint.
         */
        explicit Client(tcp::endpoint const& endpoint) {
            socket.connect(endpoint);
        }

        /**
         * @brief Send a request and read its response.
         * @param req Request to send.
         * @return Response, or std::nullopt when the write or read failed.
         */
        auto Send(HttpRequest const& req) -> std::optional<HttpResponse> {
            boost::system::error_code ec;
            http::write(socket, req, ec);
            if (ec) {
                return std::nullopt;
            }
            return Read(req.method() == http::verb::head);
        }

        /**
         * @brief Send a GET request.
         * @param target Request target.
         * @param headers Extra header lines.
         * @return Response, or std::nullopt.
         */
        auto Get(
            std::string_view target,
            std::vector<std::pair<std::string, std::string>> const& headers = {}
        ) -> std::optional<HttpResponse> {
            return Send(make_request(http::verb::get, target, {}, headers));
        }

        /**
         * @brief Read one response.
         * @param head True when the request was HEAD (no body follows).
         * @return Response, or std::nullopt when the read failed.
         */
        auto Read(bool head = false) -> std::optional<HttpResponse> {
            boost::system::error_code ec;
            http::response_parser<http::string_body> parser;
            parser.body_limit(64 * 1024 * 1024);
            parser.skip(head);
            http::read(socket, buffer, parser, ec);
            if (ec) {
                return std::nullopt;
            }
            return parser.release();
        }

        /**
         * @brief Send raw bytes.
         * @param data Bytes to write.
         * @return True on success.
         */
        auto WriteRaw(std::string_view data) -> bool {
            boost::system::error_code ec;
            net::write(socket, net::buffer(data), ec);
            return !ec;
        }

        /**
         * @brief Report whether the server has closed the connection.
         * @return True when a read reports EOF or a reset.
         */
        auto IsClosedByPeer() -> bool {
            if (buffer.size() > 0) {
                return false;
            }
            std::array<char, 1> byte{};
            boost::system::error_code ec;
            socket.read_some(net::buffer(byte), ec);
            return ec == net::error::eof || ec == net::error::connection_reset || ec == net::error::connection_aborted;
        }
    };

    /**
     * @brief Unique empty directory under the system temp directory, removed on destruction.
     */
    struct TempDir {
        std::filesystem::path path; ///< Directory path.

        TempDir() : path{ std::filesystem::temp_directory_path() / std::format("mhttp-test-{}", std::chrono::steady_clock::now().time_since_epoch().count()) } {
            std::filesystem::create_directories(path);
        }

        TempDir(TempDir const&) = delete;
        auto operator=(TempDir const&) -> TempDir& = delete;

        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        /**
         * @brief Create a file with content, creating parent directories.
         * @param relative Path relative to the directory.
         * @param content File content.
         * @return Absolute path of the file.
         */
        auto Write(
            std::filesystem::path const& relative,
            std::string_view content
        ) const -> std::filesystem::path {
            auto const file{ path / relative };
            std::filesystem::create_directories(file.parent_path());
            std::ofstream out{ file, std::ios::binary };
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            return file;
        }
    };

} // namespace test_support
