/**
 * @file static_file_test.cpp
 * @brief Verify static mount selection, exact bounded file streams, validators, byte ranges, and portable paths.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Own isolated local test files until every listener, mount, and provider has been destroyed.
 */
struct StaticTree {
    std::filesystem::path path; ///< Unique temporary parent outside all mounted roots.
    /**
     * @brief Create a unique tree with separate mounted and secret directories.
     */
    StaticTree() {
        static std::atomic<unsigned> sequence{};
        path = std::filesystem::temp_directory_path() / std::format("mhttp-static-{}-{}",
            std::chrono::steady_clock::now().time_since_epoch().count(), sequence++);
        std::filesystem::create_directories(path / "public");
        Write("secret.txt", "secret outside root");
        Write("public/index.html", "home");
        Write("public/dir/index.html", "directory");
        Write("public/letters.txt", "abcdefghijklmnopqrstuvwxyz");
        Write("public/empty.txt", "");
    }
    /**
     * @brief Remove fixtures after all native handles have retired.
     */
    ~StaticTree() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        Check(!error, "all static handles close before fixture cleanup");
    }
    /**
     * @brief Write owned binary fixture content on the calling test thread.
     * @param name Relative path below the unique fixture parent.
     * @param bytes Binary data copied before return.
     */
    auto Write(std::string_view name, std::string_view bytes) -> void {
        auto file = path / std::string{ name };
        std::filesystem::create_directories(file.parent_path());
        std::ofstream output{ file, std::ios::binary };
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) { throw std::runtime_error{ "cannot write static fixture" }; }
    }
};

/**
 * @brief Start a configured static listener on an ephemeral loopback port.
 * @param server Listener retained until all test clients drain.
 * @return Chosen port or a fixture exception.
 */
auto start_static_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start static listener" }; }
    return *port;
}

/**
 * @brief Verify fallback priority, portable filename handling, MIME overrides, and bounded binary persistence.
 */
auto static_mount_selection() -> void {
    StaticTree tree;
    tree.Write("alternate/index.html", "replacement");
    tree.Write("alternate/nested.txt", "nested mount");
    tree.Write("public/a+b.txt", "literal plus");
    tree.Write("public/a b.txt", "space");
    tree.Write("public/asset.CUSTOM", "custom type");
    std::string binary;
    for (unsigned i{}; i < 4097; ++i) { binary += static_cast<char>(i % 256); }
    tree.Write("public/binary.bin", binary);
    mhttp::Runtime runtime;
    Check(runtime.Start(3).has_value(), "start static runtime");
    mhttp::server::HttpServer server{ runtime, { .max_response_bytes = 512, .stream_chunk_bytes = 17 } };
    Check(server.Mount("/files", tree.path / "alternate").has_value(), "first mount");
    Check(server.Mount("/files/", tree.path / "public", { .mime_types = { { "custom", "application/custom" } } }).has_value(),
        "canonical duplicate mount replaces root and options");
    Check(server.Mount("/files/deep", tree.path / "alternate").has_value(), "nested mount");
    Check(server.Mount("/no-index", tree.path / "public", { .index_file = "" }).has_value(), "disable directory index");
    Check(server.Get("/files/letters.txt", [](auto&, auto& response) {
        static_cast<void>(response.SetContent("explicit route", "text/plain"));
    }).has_value(), "ordinary route takes precedence");
    Check(server.Post("/files/post-only", [](auto&, auto&) {}).has_value(), "explicit method outcome takes precedence");
    Check(server.SetNotFoundHandler([](auto&, auto& response) { static_cast<void>(response.SetContent("missing", "text/plain")); }).has_value(),
        "static misses use configured not-found handler");
    Check(server.SetPostRoutingHandler([](auto&, auto& response) { static_cast<void>(response.Headers().Set("X-After", "yes")); }).has_value(),
        "static responses retain normal final hooks");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_static_server(server) };
    CheckEq(server.Mount("/frozen", tree.path / "public").error(), mhttp::Error::AlreadyRunning, "running mount rejected");
    CheckEq(server.Unmount("/files").error(), mhttp::Error::AlreadyRunning, "running unmount rejected");
    for (auto const& [target, expected] : std::vector<std::pair<std::string_view, std::string_view>>{
        std::pair{ "/files", "home" }, { "/files/", "home" }, { "/files/dir", "directory" },
        { "/files/dir/", "directory" }, { "/files/deep/", "replacement" },
        { "/files/deep/nested.txt", "nested mount" }, { "/files/letters.txt", "explicit route" },
        { "/files/a+b.txt", "literal plus" }, { "/files/a%20b.txt?unused=1", "space" },
        { "/files/deep/letters.txt", "missing" }, { "/files-other/index.html", "missing" }, { "/no-index/dir/", "missing" } }) {
        auto reply = mhttp::SyncWait(runtime, client.Get(std::string{ target }));
        Check(reply && reply->Body() == expected && reply->Header("X-After") == "yes", "mount selection and final hooks");
    }
    auto full = mhttp::SyncWait(runtime, client.Get("/files/binary.bin"));
    Check(full && full->Body() == binary && full->Header("Content-Length") == "4097"
        && !full->Headers().Has("Transfer-Encoding"), "binary file exceeds buffered bound with exact streamed framing");
    auto head = mhttp::SyncWait(runtime, client.Send(mhttp::client::Request{ mhttp::Method::Head, "/files/binary.bin" }));
    Check(full && head && head->Body().empty() && head->Header("Content-Length") == "4097"
        && full->LocalEndpoint() == head->LocalEndpoint(), "HEAD preserves file metadata and persistent socket");
    auto custom = mhttp::SyncWait(runtime, client.Get("/files/asset.CUSTOM"));
    Check(custom && custom->Header("Content-Type") == "application/custom", "mount media overrides ignore ASCII extension case");
    auto wrong = mhttp::SyncWait(runtime, client.Post("/files/binary.bin"));
    Check(wrong && wrong->Status() == mhttp::Status::MethodNotAllowed && wrong->Header("Allow") == "GET, HEAD", "static method rejection");
    auto explicit_wrong = mhttp::SyncWait(runtime, client.Get("/files/post-only"));
    Check(explicit_wrong && explicit_wrong->Status() == mhttp::Status::MethodNotAllowed && explicit_wrong->Header("Allow") == "POST",
        "static fallback preserves ordinary route 405");
    Check(server.Stop().has_value(), "stop static server");
    CheckEq(server.Unmount("/files/").value_or(false), true, "remove canonical prefix after drain");
    CheckEq(server.Unmount("/files").value_or(true), false, "absent mount removal");
    Check(!server.Mount("/bad", tree.path / "public", { .index_file = "../secret.txt" }), "unsafe index rejected");
    Check(!server.Mount("/bad", tree.path / "public", { .max_ranges = 0 }), "invalid range bound rejected");
    Check(!server.Mount("/bad", tree.path / "public", { .mime_types = { { "txt", "x\r\nInjected: yes" } } }), "unsafe MIME rejected");
    Check(!server.Mount("/bad", tree.path / "public/letters.txt"), "file cannot be a mount root");
    Check(!server.Mount("/../bad", tree.path / "public"), "unsafe mount prefix rejected");
}

/**
 * @brief Check a broad byte-range boundary table and multipart byte counts without buffering files on the server.
 */
auto static_byte_ranges() -> void {
    StaticTree tree;
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start range runtime");
    mhttp::server::HttpServer server{ runtime, { .max_response_bytes = 512, .stream_chunk_bytes = 7 } };
    Check(server.Mount("/", tree.path / "public", { .max_ranges = 4 }).has_value(), "root mount with range cap");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_static_server(server) };
    struct RangeCase {
        std::string_view value; ///< Raw bounded Range value.
        unsigned status; ///< Expected wire status.
        std::string_view bytes; ///< Expected selected representation for 200/206.
        std::string_view content_range; ///< Exact Content-Range, empty when omitted.
    };
    for (auto const& test : std::vector<RangeCase>{
        RangeCase{ "bytes=0-0", 206, "a", "bytes 0-0/26" }, { "bytes=1-3", 206, "bcd", "bytes 1-3/26" },
        { "bytes=25-25", 206, "z", "bytes 25-25/26" }, { "bytes=24-", 206, "yz", "bytes 24-25/26" },
        { "bytes=-3", 206, "xyz", "bytes 23-25/26" }, { "bytes=-26", 206, "abcdefghijklmnopqrstuvwxyz", "bytes 0-25/26" },
        { "bytes=-999999999999999999999999", 206, "abcdefghijklmnopqrstuvwxyz", "bytes 0-25/26" },
        { "bytes=00001-00003", 206, "bcd", "bytes 1-3/26" }, { "Bytes=1-2", 206, "bc", "bytes 1-2/26" },
        { "bytes=24-99999999999999999999999", 206, "yz", "bytes 24-25/26" },
        { "bytes=26-", 416, "", "bytes */26" }, { "bytes=999999999999999999999-", 416, "", "bytes */26" },
        { "bytes=-0", 416, "", "bytes */26" }, { "bytes=40-50,26-27,-0", 416, "", "bytes */26" },
        { "bytes=40-50,0-1", 206, "ab", "bytes 0-1/26" }, { "bytes=0-0,1-1,2-2,3-3,4-4", 416, "", "bytes */26" },
        { "items=0-1", 200, "abcdefghijklmnopqrstuvwxyz", "" }, { "bytes=3-1", 200, "abcdefghijklmnopqrstuvwxyz", "" },
        { "bytes=+1-2", 200, "abcdefghijklmnopqrstuvwxyz", "" }, { "bytes=1--2", 200, "abcdefghijklmnopqrstuvwxyz", "" },
        { "bytes=0-1,", 200, "abcdefghijklmnopqrstuvwxyz", "" }, { "bytes=", 200, "abcdefghijklmnopqrstuvwxyz", "" },
        { "bytes=1 -2", 200, "abcdefghijklmnopqrstuvwxyz", "" }, { "bytes=1-2x", 200, "abcdefghijklmnopqrstuvwxyz", "" },
        { "bytes=99999999999999999999999-9999999999999999999999", 200, "abcdefghijklmnopqrstuvwxyz", "" } }) {
        mhttp::client::Request request{ mhttp::Method::Get, "/letters.txt" };
        Check(request.Headers().Set("Range", test.value).has_value(), "valid raw range field value");
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        if (!Check(reply.has_value(), "range request completes")) { continue; }
        CheckEq(static_cast<unsigned>(reply->Status()), test.status, test.value);
        CheckEq(reply->Header("Content-Range"), test.content_range, "exact range metadata");
        if (test.status != 416) { CheckEq(reply->Body(), test.bytes, "selected raw file bytes"); }
    }
    for (auto ranges : { "bytes=25-25,0-1", "bytes=0-2,1-3", "bytes= 0-0, 25-25" }) {
        mhttp::client::Request request{ mhttp::Method::Get, "/letters.txt" };
        Check(request.Headers().Set("Range", ranges).has_value(), "valid multipart range field value");
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        if (!Check(reply && reply->Status() == mhttp::Status::PartialContent,
            std::format("multiple ranges use 206: {} ({})", ranges, reply ? std::to_string(static_cast<unsigned>(reply->Status()))
                : std::string{ mhttp::ToString(reply.error()) }))) { continue; }
        auto media = reply->Header("Content-Type");
        if (!Check(media.starts_with("multipart/byteranges; boundary="), "multipart media boundary")) { continue; }
        auto boundary = media.substr(media.find('=') + 1);
        auto const reversed = std::string_view{ ranges }.contains("25-25,0-1");
        auto const overlap = std::string_view{ ranges }.contains("0-2,1-3");
        auto part = [&](std::string_view range, std::string_view bytes) {
            return std::format("--{}\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Range: bytes {}/26\r\n\r\n{}\r\n",
                boundary, range, bytes);
        };
        auto expected = reversed ? part("25-25", "z") + part("0-1", "ab")
            : overlap ? part("0-2", "abc") + part("1-3", "bcd") : part("0-0", "a") + part("25-25", "z");
        expected += std::format("--{}--\r\n", boundary);
        CheckEq(reply->Body(), expected, "multipart preserves range order, overlap, delimiters, and every byte");
        CheckEq(reply->Header("Content-Length"), std::to_string(expected.size()), "exact multipart representation length");
        Check(!reply->Headers().Has("Content-Range"), "multipart has only per-part Content-Range");
    }
    for (auto target : { "/letters.txt", "/empty.txt" }) {
        mhttp::client::Request request{ mhttp::Method::Head, target };
        static_cast<void>(request.Headers().Set("Range", "bytes=999-"));
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(reply && reply->Status() == mhttp::Status::Ok && reply->Body().empty() && !reply->Headers().Has("Content-Range"),
            "HEAD ignores Range and suppresses all file bytes");
    }
    mhttp::client::Request empty{ mhttp::Method::Get, "/empty.txt" };
    static_cast<void>(empty.Headers().Set("Range", "bytes=0-"));
    auto reply = mhttp::SyncWait(runtime, client.Send(std::move(empty)));
    Check(reply && reply->Status() == mhttp::Status::Ok && reply->Body().empty(), "empty representation ignores ranges");
    Check(server.Stop().has_value(), "drain range streams");
}

/**
 * @brief Verify conditional request precedence and conservative range fallback with weak metadata validators.
 */
auto static_conditional_requests() -> void {
    StaticTree tree;
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start conditional runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Mount("/", tree.path / "public").has_value(), "conditional mount");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_static_server(server) };
    auto first = mhttp::SyncWait(runtime, client.Get("/letters.txt"));
    if (!Check(first.has_value(), "get validators")) { return; }
    auto const etag = std::string{ first->Header("ETag") }, modified = std::string{ first->Header("Last-Modified") };
    Check(etag.starts_with("W/\"") && mhttp::ParseHttpDate(modified).has_value(), "weak metadata tag and valid UTC date");
    for (auto const& fields : std::vector<std::vector<std::pair<std::string, std::string>>>{
        { { "If-None-Match", etag } }, { { "If-None-Match", etag.substr(2) } }, { { "If-None-Match", "*" } },
        { { "If-None-Match", "\"other,opaque\", " + etag } }, { { "If-None-Match", "\"other\"" }, { "If-None-Match", etag } },
        { { "If-Modified-Since", modified } }, { { "If-None-Match", etag }, { "Range", "bytes=999-" } } }) {
        mhttp::client::Request request{ mhttp::Method::Get, "/letters.txt" };
        for (auto const& [name, value] : fields) { static_cast<void>(request.Headers().Add(name, value)); }
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(reply && reply->Status() == mhttp::Status::NotModified && reply->Body().empty()
            && reply->Header("ETag") == etag && !reply->Headers().Has("Content-Length"), "matching condition returns metadata-only 304 before ranges");
    }
    for (auto const& fields : std::vector<std::vector<std::pair<std::string, std::string>>>{
        { { "If-None-Match", "\"other\"" }, { "If-Modified-Since", modified } },
        { { "If-None-Match", etag + ", invalid" } }, { { "If-Modified-Since", "not a date" } },
        { { "If-Modified-Since", "Thu, 01 Jan 1970 00:00:00 GMT" } },
        { { "Range", "bytes=0-1" }, { "If-Range", etag } }, { { "Range", "bytes=0-1" }, { "If-Range", modified } } }) {
        mhttp::client::Request request{ mhttp::Method::Get, "/letters.txt" };
        for (auto const& [name, value] : fields) { static_cast<void>(request.Headers().Add(name, value)); }
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(reply && reply->Status() == mhttp::Status::Ok && reply->Body() == first->Body(), "nonmatching, invalid, or weak If-Range conditions select full representation");
    }
    for (auto const& [name, value] : std::vector<std::pair<std::string_view, std::string_view>>{ { "If-Match", "\"unknown\"" },
        { "If-Unmodified-Since", "Thu, 01 Jan 1970 00:00:00 GMT" } }) {
        mhttp::client::Request request{ mhttp::Method::Get, "/letters.txt" };
        static_cast<void>(request.Headers().Set(name, value));
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(reply && reply->Status() == mhttp::Status::PreconditionFailed, "failed write-style precondition is not silently ignored");
    }
    tree.Write("public/letters.txt", "changed representation");
    mhttp::client::Request changed{ mhttp::Method::Get, "/letters.txt" };
    static_cast<void>(changed.Headers().Set("If-None-Match", etag));
    auto next = mhttp::SyncWait(runtime, client.Send(std::move(changed)));
    Check(next && next->Status() == mhttp::Status::Ok && next->Body() == "changed representation" && next->Header("ETag") != etag,
        "file mutation invalidates the prior weak validator");
    Check(server.Stop().has_value(), "drain conditional streams");
}

/**
 * @brief Preserve route leases, authentication order, and nonblocking configuration guards.
 */
auto static_mount_guards() -> void {
    StaticTree tree;
    mhttp::server::Router router;
    Check(router.Mount("/", tree.path / "public").has_value(), "standalone static mount");
    {
        auto request = mhttp::detail::ServerRequestAccess::Create({ .target = "/letters.txt" });
        mhttp::server::Response response;
        auto pending = mhttp::detail::RouterAccess::Prepare(router, request, response);
        CheckEq(router.Mount("/other", tree.path / "public").error(), mhttp::Error::Busy, "unstarted preparation prevents mount changes");
        CheckEq(router.Unmount("/").error(), mhttp::Error::Busy, "unstarted preparation prevents unmount");
    }
    Check(router.Mount("/", tree.path / "public").has_value(), "dropped preparation releases configuration lease");
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start static guard runtime");
    mhttp::server::HttpServer server{ runtime };
    auto auth = mhttp::server::BearerAuth::Create("file-token", "files");
    Check(auth && server.Use(std::move(*auth)), "guard static fallback with global authentication");
    Check(server.Mount("/", tree.path / "public").has_value(), "guarded mount");
    Check(!server.Mount("/", tree.path / "missing"), "failed replacement preserves existing mount");
    Check(server.Get("/io", [&](auto&, auto& response) {
        CheckEq(router.Mount("/io", tree.path / "public").error(), mhttp::Error::Busy, "filesystem configuration refuses I/O threads");
        static_cast<void>(response.SetContent("guarded", "text/plain"));
    }).has_value(), "I/O configuration probe");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_static_server(server) };
    auto denied = mhttp::SyncWait(runtime, client.Get("/letters.txt"));
    Check(denied && denied->Status() == mhttp::Status::Unauthorized && !denied->Headers().Has("ETag")
        && denied->Header("WWW-Authenticate").starts_with("Bearer"), "global Before denies static content before representation selection");
    for (auto target : { "/letters.txt", "/io" }) {
        mhttp::client::Request request{ mhttp::Method::Get, target };
        static_cast<void>(request.Headers().Set("Authorization", "Bearer file-token"));
        auto reply = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(reply && reply->Status() == mhttp::Status::Ok
            && reply->Body() == (std::string_view{ target } == "/io" ? "guarded" : "abcdefghijklmnopqrstuvwxyz"),
            "authorized fallback and explicit route retain normal global middleware");
    }
    Check(server.Stop().has_value(), "drain guarded mount");
}

/**
 * @brief Run static response integration cases.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Test process exit status.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "static mount selection", static_mount_selection },
        { "static byte ranges", static_byte_ranges },
        { "static conditional requests", static_conditional_requests },
        { "static mount configuration and authentication", static_mount_guards },
    });
}
