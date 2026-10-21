/**
 * @file concurrency_test.cpp
 * @brief Run simultaneous real HTTP CRUD clients against synchronized application state on a multithreaded Runtime.
 */
import std;
import mhttp.json;

#include "../support/test_support.hpp"
#include "rest_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;

/**
 * @brief Require a complete HTTP response and return its independent JSON value for application checks.
 * @param result Owned completed exchange.
 * @param status Expected HTTP status.
 * @return Owned JSON representation, or an exception recorded by the worker boundary.
 */
auto require_json(
    mhttp::Result<mhttp::client::Response> result,
    mhttp::Status status
) -> mhttp::Json {
    if (!result || result->Status() != status) { throw std::runtime_error{ "unexpected concurrent REST response" }; }
    auto value = result->Json();
    if (!value) { throw std::runtime_error{ "invalid concurrent REST JSON" }; }
    return std::move(*value);
}

/**
 * @brief Keep client connections independent while validating CRUD state and globally unique resource identifiers.
 */
auto concurrent_user_crud() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start four-thread REST runtime");
    mhttp::server::HttpServer server{ runtime };
    auto store = std::make_shared<mhttp::test::UserStore>();
    mhttp::test::register_user_routes(server, store);
    auto port = mhttp::test::start_rest_peer(server);
    constexpr unsigned CLIENTS{ 8 }, ROUNDS{ 10 };
    std::mutex observations;
    std::vector<std::uint64_t> created_ids;
    std::map<std::uint64_t, std::string> retained;
    std::latch start{ CLIENTS };
    std::vector<std::jthread> workers;
    for (unsigned worker{}; worker < CLIENTS; ++worker) {
        workers.emplace_back([&, worker] {
            start.arrive_and_wait();
            try {
                mhttp::client::HttpClient client{ runtime, "127.0.0.1", port };
                for (unsigned round{}; round < ROUNDS; ++round) {
                    auto name = std::format("worker {} round {}", worker, round);
                    auto created = require_json(mhttp::SyncWait(runtime, client.Post("/users", mhttp::Json{
                        { "name", name }, { "age", round } })), mhttp::Status::Created);
                    auto id = created["id"].get<std::uint64_t>();
                    auto path = "/users/" + std::to_string(id);
                    {
                        std::lock_guard lock{ observations };
                        created_ids.push_back(id);
                    }
                    auto read = require_json(mhttp::SyncWait(runtime, client.Get(path)), mhttp::Status::Ok);
                    Check(read == created, "concurrent GET sees its own committed creation");
                    auto replaced = require_json(mhttp::SyncWait(runtime, client.Put(path, mhttp::Json{
                        { "name", name + " replaced" }, { "age", 40 } })), mhttp::Status::Ok);
                    Check(replaced["id"] == id && replaced["name"] == name + " replaced" && replaced["age"] == 40, "concurrent replacement retains resource identity");
                    auto patched = require_json(mhttp::SyncWait(runtime, client.Patch(path, mhttp::Json{ { "age", 41 } })), mhttp::Status::Ok);
                    Check(patched["age"] == 41 && patched["name"] == name + " replaced", "concurrent patch preserves the other committed field");
                    auto removed = mhttp::SyncWait(runtime, client.Delete(path));
                    Check(removed && removed->Status() == mhttp::Status::NoContent && removed->Body().empty(), "each client deletes only its own resource");
                    auto absent = mhttp::SyncWait(runtime, client.Get(path));
                    Check(absent && absent->Status() == mhttp::Status::NotFound, "deleted resource cannot reappear in a concurrent read");
                }
                auto name = std::format("retained worker {}", worker);
                auto final = require_json(mhttp::SyncWait(runtime, client.Post("/users", mhttp::Json{
                    { "name", name }, { "age", worker } })), mhttp::Status::Created);
                auto id = final["id"].get<std::uint64_t>();
                std::lock_guard lock{ observations };
                created_ids.push_back(id);
                retained.emplace(id, std::move(name));
            } catch (std::exception const& error) {
                Check(false, std::format("REST worker {}: {}", worker, error.what()));
            }
        });
    }
    workers.clear();
    CheckEq(created_ids.size(), static_cast<std::size_t>(CLIENTS * (ROUNDS + 1)), "all clients complete every creation");
    std::ranges::sort(created_ids);
    Check(std::ranges::adjacent_find(created_ids) == created_ids.end(), "concurrent creations never reuse an identifier");
    auto final = store->Snapshot();
    CheckEq(final.size(), static_cast<std::size_t>(CLIENTS), "only the eight deliberately retained resources remain");
    for (auto const& user : final) {
        auto expected = retained.find(user.id);
        Check(expected != retained.end() && expected->second == user.name, "final synchronized state matches every client commit");
    }
    mhttp::client::HttpClient observer{ runtime, "127.0.0.1", port };
    auto collection = require_json(mhttp::SyncWait(runtime, observer.Get("/users?size=100")), mhttp::Status::Ok);
    Check(collection["total"] == CLIENTS && collection["items"].size() == CLIENTS, "HTTP collection agrees with the synchronized final state");
    for (auto const& item : collection["items"]) {
        auto expected = retained.find(item["id"].get<std::uint64_t>());
        Check(expected != retained.end() && item["name"] == expected->second, "final HTTP listing contains exactly the retained application resources");
    }
}

/**
 * @brief Run concurrent REST acceptance with an outer watchdog and loopback-only peers.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, { { "concurrent REST user CRUD", concurrent_user_crud } });
}
