/**
 * @file rest_crud_test.cpp
 * @brief Exercise full JSON user CRUD, typed path validation, method dispatch, pagination, and filtering over HTTP.
 */
import std;
import mhttp.json;

#include "../support/test_support.hpp"
#include "rest_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;

/**
 * @brief Execute one full user lifecycle and leave deleted resources absent.
 */
auto rest_user_lifecycle() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start REST CRUD runtime");
    mhttp::server::HttpServer server{ runtime };
    auto store = std::make_shared<mhttp::test::UserStore>();
    mhttp::test::register_user_routes(server, store);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", mhttp::test::start_rest_peer(server) };
    auto missing = mhttp::SyncWait(runtime, client.Get("/users/1"));
    Check(missing && missing->Status() == mhttp::Status::NotFound, "unknown user starts as 404");
    auto created = mhttp::SyncWait(runtime, client.Post("/users", mhttp::Json{ { "name", "Ada 你好" }, { "age", 36 } }));
    if (!Check(created && created->Status() == mhttp::Status::Created, "POST creates user with 201")) { return; }
    auto user = created->Json();
    Check(user && (*user)["id"] == 1 && (*user)["name"] == "Ada 你好" && (*user)["age"] == 36, "created representation owns validated user values");
    auto location = std::string{ created->Header("Location") };
    CheckEq(location, "/users/1", "201 publishes canonical resource location");
    auto fetched = mhttp::SyncWait(runtime, client.Get(location));
    Check(fetched && fetched->Status() == mhttp::Status::Ok && fetched->Json() == user, "GET retrieves created representation");
    auto replaced = mhttp::SyncWait(runtime, client.Put(location, mhttp::Json{ { "name", "Grace" }, { "age", 37 } }));
    Check(replaced && replaced->Status() == mhttp::Status::Ok && (*replaced->Json())["name"] == "Grace"
        && (*replaced->Json())["age"] == 37, "PUT replaces complete user data");
    auto patched = mhttp::SyncWait(runtime, client.Patch(location, mhttp::Json{ { "age", 38 } }));
    Check(patched && (*patched->Json())["name"] == "Grace" && (*patched->Json())["age"] == 38, "PATCH changes one field and preserves omitted fields");
    auto invalid = mhttp::SyncWait(runtime, client.Patch(location, mhttp::Json{ { "age", -1 } }));
    auto unchanged = mhttp::SyncWait(runtime, client.Get(location));
    Check(invalid && invalid->Status() == mhttp::Status::BadRequest && unchanged && unchanged->Json() == patched->Json(), "invalid update is 400 and leaves the resource unchanged");
    auto incomplete = mhttp::SyncWait(runtime, client.Put(location, mhttp::Json{ { "age", 40 } }));
    auto empty_patch = mhttp::SyncWait(runtime, client.Patch(location, mhttp::Json::object()));
    Check(incomplete && incomplete->Status() == mhttp::Status::BadRequest
        && empty_patch && empty_patch->Status() == mhttp::Status::BadRequest, "PUT requires complete data and PATCH requires a changed field");
    auto mismatch = mhttp::SyncWait(runtime, client.Options(location));
    Check(mismatch && mismatch->Status() == mhttp::Status::MethodNotAllowed
        && mismatch->Header("Allow").contains("GET") && mismatch->Header("Allow").contains("PATCH"), "unregistered resource method is 405 with Allow");
    for (auto path : { "/users/not-an-id", "/users/0", "/users/-1", "/users/18446744073709551616" }) {
        auto result = mhttp::SyncWait(runtime, client.Get(path));
        Check(result && result->Status() == mhttp::Status::BadRequest, "strict typed path conversion rejects invalid and overflowing identifiers");
    }
    auto removed = mhttp::SyncWait(runtime, client.Delete(location));
    Check(removed && removed->Status() == mhttp::Status::NoContent && removed->Body().empty(), "DELETE returns empty 204");
    auto absent = mhttp::SyncWait(runtime, client.Get(location));
    Check(absent && absent->Status() == mhttp::Status::NotFound && store->Snapshot().empty(), "deleted user stays absent from HTTP and synchronized state");
}

/**
 * @brief Page and filter ordered application snapshots with strict query and body validation.
 */
auto rest_user_pagination() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start REST pagination runtime");
    mhttp::server::HttpServer server{ runtime };
    auto store = std::make_shared<mhttp::test::UserStore>();
    mhttp::test::register_user_routes(server, store);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", mhttp::test::start_rest_peer(server) };
    for (unsigned index{}; index < 25; ++index) {
        auto result = mhttp::SyncWait(runtime, client.Post("/users", mhttp::Json{
            { "name", std::format("{} user {}", index % 2 == 0 ? "even" : "odd", index) }, { "age", index } }));
        Check(result && result->Status() == mhttp::Status::Created, "populate ordered application resources through HTTP");
    }
    mhttp::QueryParams query;
    query.Add("page", "2");
    query.Add("size", "5");
    query.Add("name", "even user");
    auto result = mhttp::SyncWait(runtime, client.Get("/users", query));
    if (!Check(result.has_value(), "retrieve filtered page")) { return; }
    auto page = result->Json();
    Check(page && (*page)["page"] == 2 && (*page)["size"] == 5 && (*page)["total"] == 13, "query parameters filter before pagination");
    if (page) {
        auto const& items = (*page)["items"];
        Check(items.size() == 5 && items[0]["id"] == 11 && items[4]["id"] == 19, "second page preserves stable resource order");
    }
    for (auto target : { "/users?page=0", "/users?size=0", "/users?size=101", "/users?page=bad", "/users?size=-1" }) {
        auto invalid = mhttp::SyncWait(runtime, client.Get(target));
        Check(invalid && invalid->Status() == mhttp::Status::BadRequest, "invalid typed pagination is 400");
    }
    for (auto const& body : { mhttp::Json::array(), mhttp::Json{ { "name", 42 }, { "age", 1 } },
                              mhttp::Json{ { "name", "valid" }, { "age", 151 } } }) {
        auto invalid = mhttp::SyncWait(runtime, client.Post("/users", body));
        Check(invalid && invalid->Status() == mhttp::Status::BadRequest, "invalid JSON root and field schema cannot create resources");
    }
    CheckEq(store->Snapshot().size(), std::size_t{ 25 }, "rejected inputs leave collection cardinality unchanged");
}

/**
 * @brief Run real REST acceptance as an optional-JSON consumer.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Bounded test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "REST user CRUD lifecycle", rest_user_lifecycle },
        { "REST user pagination and filtering", rest_user_pagination },
    });
}
