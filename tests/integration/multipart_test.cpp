/**
 * @file multipart_test.cpp
 * @brief Round-trip multipart files/fields over public HTTP and map malformed/oversized forms to explicit responses.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Start one configured upload listener on a dynamic loopback port.
 * @param server Longer-lived listener.
 * @return Chosen port, or an exception on fixture startup failure.
 */
auto start_multipart_peer(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start multipart peer" }; }
    return *port;
}

/**
 * @brief Compare all ordered part bytes and metadata in independently owned parsed forms.
 * @param actual Borrowed response form.
 * @param expected Borrowed independently retained original form.
 */
auto check_multipart_parts(mhttp::FormData const& actual, mhttp::FormData const& expected) -> void {
    if (!CheckEq(actual.Size(), expected.Size(), "HTTP preserves multipart part count")) { return; }
    for (std::size_t index{}; index < actual.Size(); ++index) {
        auto const& left = actual.Parts()[index];
        auto const& right = expected.Parts()[index];
        CheckEq(left.name, right.name, "HTTP preserves ordered/repeated field names");
        CheckEq(left.content, right.content, "HTTP preserves every binary content byte");
        CheckEq(left.filename, right.filename, "HTTP preserves optional file metadata");
        CheckEq(left.content_type, right.content_type, "HTTP preserves explicit part media declarations");
    }
}

/**
 * @brief Round-trip twenty deterministic random binary forms and one large upload across asynchronous callbacks.
 */
auto multipart_http_round_trips() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start multipart round-trip runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Post("/echo", [](auto& request, auto& response) -> mhttp::Task<void> {
        auto form = request.Multipart();
        if (!form) {
            Check(response.SetError(form.error() == mhttp::Error::BodyTooLarge
                ? mhttp::Status::ContentTooLarge : mhttp::Status::BadRequest).has_value(), "map multipart parse failure");
            co_return;
        }
        co_await mhttp::Sleep(std::chrono::milliseconds{ 1 });
        auto encoded = form->Encode();
        Check(encoded.has_value(), "re-encode request-owned form after asynchronous suspension");
        if (encoded) { Check(response.SetContent(std::move(encoded->body), encoded->content_type).has_value(), "echo multipart representation"); }
    }).has_value(), "register asynchronous multipart echo");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_multipart_peer(server) };
    std::mt19937 random{ 0x75882026 };
    std::optional<mhttp::Endpoint> connection;
    for (unsigned round{}; round <= 20; ++round) {
        mhttp::FormData expected;
        auto count = round == 20 ? 2U : 2U + random() % 7;
        for (unsigned index{}; index < count; ++index) {
            bool file = index % 2 == 1;
            auto size = round == 20 ? 2U * 1024 * 1024 : random() % 4096;
            std::string bytes(size, '\0');
            for (char& byte : bytes) { byte = static_cast<char>(random() & 255); }
            expected.Add({ .name = file ? "files" : index < 3 ? "field" : "field-" + std::to_string(index), .content = std::move(bytes),
                .filename = file ? std::optional{ "file-" + std::to_string(index) + ".bin" } : std::nullopt,
                .content_type = file ? "application/octet-stream" : "text/plain; charset=unknown" });
        }
        mhttp::client::Request request{ mhttp::Method::Post, "/echo" };
        {
            auto temporary = expected;
            Check(request.SetMultipart(temporary).has_value(), "serialize independently owned request before source destruction");
        }
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        if (!Check(result && result->Status() == mhttp::Status::Ok, "real multipart HTTP exchange succeeds")) { continue; }
        auto actual = mhttp::FormData::Parse(result->Body(), result->ContentType());
        if (Check(actual.has_value(), "decode independently owned response form")) { check_multipart_parts(*actual, expected); }
        if (connection) { CheckEq(result->LocalEndpoint(), *connection, "framed multipart responses retain keep-alive reuse"); }
        else { connection = result->LocalEndpoint(); }
    }
}

/**
 * @brief Exercise explicit handler error mapping, configurable field count, and per-file/per-field limits over HTTP.
 */
auto multipart_http_limits() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start multipart admission runtime");
    mhttp::server::HttpServer server{ runtime };
    for (auto path : { "/default", "/small", "/wide" }) {
        Check(server.Post(path, [path](auto& request, auto& response) {
            mhttp::MultipartLimits limits;
            if (std::string_view{ path } == "/small") { limits.max_parts = 2; limits.part_bytes = 4; }
            if (std::string_view{ path } == "/wide") { limits.max_parts = 129; }
            auto form = request.Multipart(limits);
            if (!form) {
                Check(response.SetError(form.error() == mhttp::Error::BodyTooLarge
                    ? mhttp::Status::ContentTooLarge : mhttp::Status::BadRequest).has_value(), "map multipart admission error");
            } else { Check(response.SetEmpty(mhttp::Status::NoContent).has_value(), "accept bounded multipart input"); }
        }).has_value(), "register upload with explicit admission policy");
    }
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_multipart_peer(server) };
    for (bool file : { false, true }) {
        for (unsigned bytes : { 4U, 5U }) {
            mhttp::FormData form;
            form.Add({ .name = "data", .content = std::string(bytes, 'x'), .filename = file ? std::optional<std::string>{ "a.bin" } : std::nullopt });
            mhttp::client::Request request{ mhttp::Method::Post, "/small" };
            Check(request.SetMultipart(form).has_value(), "construct upload before stricter server admission");
            auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
            Check(result && result->Status() == (bytes == 4 ? mhttp::Status::NoContent : mhttp::Status::ContentTooLarge), "same per-part limit applies to ordinary fields and uploaded files");
        }
    }
    mhttp::FormData many;
    for (unsigned index{}; index < 129; ++index) { many.Add({ .name = "field", .content = "x" }); }
    for (auto path : { "/default", "/wide" }) {
        mhttp::client::Request request{ mhttp::Method::Post, path };
        Check(request.SetMultipart(many, {}, { .max_parts = 129 }).has_value(), "explicitly permit the outgoing field count");
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(result && result->Status() == (std::string_view{ path } == "/default"
            ? mhttp::Status::ContentTooLarge : mhttp::Status::NoContent), "default 128-part bound and configured override reach HTTP responses");
    }
    std::string excessive = "--b\r\nContent-Disposition: form-data; name=x\r\n";
    for (unsigned index{}; index < 32; ++index) { excessive += "X-Ignored-" + std::to_string(index) + ": x\r\n"; }
    excessive += "\r\nx\r\n--b--\r\n";
    for (auto const& body : { excessive, std::string{ "--b\r\nContent-Disposition: form-data; name=x\r\nBad Field: x\r\n\r\ny\r\n--b--\r\n" } }) {
        auto rejected = mhttp::SyncWait(runtime, client.Post("/default", body, "multipart/form-data; boundary=b"));
        Check(rejected && rejected->Status() == mhttp::Status::BadRequest, "malformed or excessive part headers produce 400");
    }
    mhttp::FormData valid;
    valid.Add({ .name = "after-error", .content = "ok" });
    mhttp::client::Request request{ mhttp::Method::Post, "/default" };
    Check(request.SetMultipart(valid).has_value(), "construct valid upload after semantic MIME errors");
    auto recovered = mhttp::SyncWait(runtime, client.Send(std::move(request)));
    Check(recovered && recovered->Status() == mhttp::Status::NoContent, "fully consumed MIME rejection does not corrupt the next HTTP request");
}

/**
 * @brief Run multipart HTTP acceptance using only the public module.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Bounded test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "multipart HTTP randomized binary round trips", multipart_http_round_trips },
        { "multipart HTTP admission and error mapping", multipart_http_limits },
    });
}
