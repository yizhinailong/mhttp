/**
 * @file main.cpp
 * @brief Exercise a complete note CRUD lifecycle through the public owning HTTP client.
 */
import std;
import mhttp;

namespace {

    /**
     * @brief Report transport errors separately from unexpected HTTP statuses.
     * @param result Owned completed client result.
     * @param expected Required HTTP status.
     * @return Owned response, including headers needed by the next request.
     */
    auto expect(
        mhttp::Result<mhttp::client::Response> result,
        mhttp::Status expected
    ) -> mhttp::client::Response {
        if (!result) { throw std::runtime_error{ std::string{ mhttp::ToString(result.error()) } }; }
        std::cout << static_cast<unsigned>(result->Status()) << ' ' << result->Body() << '\n';
        if (result->Status() != expected) { throw std::runtime_error{ "unexpected HTTP status" }; }
        return std::move(*result);
    }

    /**
     * @brief Check returned content so a successful status alone cannot hide an incomplete CRUD sequence.
     * @param response Borrowed owned response.
     * @param expected Exact representation bytes.
     */
    auto expect_body(mhttp::client::Response const& response, std::string_view expected) -> void {
        if (response.Body() != expected) { throw std::runtime_error{ "unexpected note content" }; }
    }

} // namespace

/**
 * @brief Run requests sequentially and block only this non-I/O coordinator thread.
 * @param argc Argument count.
 * @param argv Optional HTTP origin of the companion rest_server.
 * @return Zero when every request meets its expected status and representation.
 */
auto main(int argc, char* argv[]) -> int {
    using namespace std::chrono_literals;
    try {
        if (argc > 2) { throw std::runtime_error{ "usage: rest_client [http://host:port]" }; }
        mhttp::Runtime runtime;
        auto started = runtime.Start(2);
        if (!started) { throw std::runtime_error{ std::string{ mhttp::ToString(started.error()) } }; }
        mhttp::client::HttpClient client{ runtime, argc == 2 ? argv[1] : "http://127.0.0.1:8080",
            { .timeout_policy = mhttp::client::TimeoutPolicy::Overall, .timeout = 5s } };
        expect(mhttp::SyncWait(runtime, client.Get("/health")), mhttp::Status::Ok);
        auto created = expect(mhttp::SyncWait(runtime, client.Post("/notes", "first note", "text/plain")),
            mhttp::Status::Created);
        auto const path = std::string{ created.Header("Location") };
        if (!path.starts_with("/notes/")) { throw std::runtime_error{ "missing note Location" }; }
        expect_body(expect(mhttp::SyncWait(runtime, client.Get(path)), mhttp::Status::Ok), "first note");
        expect_body(expect(mhttp::SyncWait(runtime, client.Patch(path, "updated note", "text/plain")),
            mhttp::Status::Ok), "updated note");
        mhttp::QueryParams query;
        query.Add("limit", "1");
        expect(mhttp::SyncWait(runtime, client.Get("/notes", std::move(query))), mhttp::Status::Ok);
        expect_body(expect(mhttp::SyncWait(runtime, client.Put(path, "final note", "text/plain")),
            mhttp::Status::Ok), "final note");
        expect_body(expect(mhttp::SyncWait(runtime, client.Get(path)), mhttp::Status::Ok), "final note");
        expect_body(expect(mhttp::SyncWait(runtime, client.Head(path)), mhttp::Status::Ok), "");
        auto options = expect(mhttp::SyncWait(runtime, client.Options(path)), mhttp::Status::NoContent);
        if (options.Header("Allow").empty()) { throw std::runtime_error{ "OPTIONS omitted Allow" }; }
        expect_body(expect(mhttp::SyncWait(runtime, client.Delete(path)), mhttp::Status::NoContent), "");
        expect(mhttp::SyncWait(runtime, client.Get(path)), mhttp::Status::NotFound);
        expect(mhttp::SyncWait(runtime, client.Post("/notes", "", "text/plain")), mhttp::Status::BadRequest);
        expect(mhttp::SyncWait(runtime, client.Get("/notes?limit=invalid")), mhttp::Status::BadRequest);
        std::cout << "CRUD completed.\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
