/**
 * @file soci_test.cpp
 * @brief Exercise the optional SOCI session pool as a consumer importing only the public mhttp.soci module.
 */
import std;
import mhttp.soci;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

/**
 * @brief Require one operation to fail with a specific mhttp error carried by std::system_error.
 * @tparam Function Synchronous callable expected to throw.
 * @param function Operation invoked once.
 * @param expected Required error value.
 * @param message Expectation diagnostic.
 */
template <typename Function>
auto expect_error(Function function, mhttp::Error expected, std::string_view message) -> void {
    try {
        function();
        Check(false, message);
    } catch (std::system_error const& error) {
        CheckEq(error.code(), mhttp::make_error_code(expected), message);
    }
}

/**
 * @brief Own a uniquely named SQLite file shared by several sessions and remove it after the test.
 * @note Destroy every session using the file first; Windows cannot delete an open database.
 */
class TemporaryDatabase {
private:
    std::filesystem::path m_path; ///< Database file that SQLite creates on first connection.

public:
    /**
     * @brief Choose a fresh file name in the system temporary directory.
     */
    TemporaryDatabase() :
        m_path{ std::filesystem::temp_directory_path() / std::format("mhttp-soci-{}.db", std::random_device{}()) } {}

    /**
     * @brief Prevent two owners from removing one file.
     * @param other Unused source.
     */
    TemporaryDatabase(TemporaryDatabase const& other) = delete;

    /**
     * @brief Prevent two owners from removing one file.
     * @param other Unused source.
     * @return Not callable.
     */
    auto operator=(TemporaryDatabase const& other) -> TemporaryDatabase& = delete;

    /**
     * @brief Remove the database and any rollback journal left by an interrupted write.
     */
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
        std::filesystem::remove(std::filesystem::path{ m_path } += "-journal", ignored);
    }

    /**
     * @brief Build a SQLite connection string with a busy wait for writes from other sessions.
     * @return Owned SOCI connection string quoting the database path.
     */
    [[nodiscard]] auto ConnectString() const -> std::string {
        return std::format("db=\"{}\" timeout=5 synchronous=off", m_path.generic_string());
    }
};

/**
 * @brief Attempt to connect from inside a Runtime I/O thread, where blocking connection setup is refused.
 * @return The recoverable error, or nullopt if the pool unexpectedly opened.
 */
auto open_on_io_thread() -> mhttp::Task<std::optional<mhttp::Error>> {
    auto pool = mhttp::db::SessionPool::Open(soci::sqlite3, ":memory:");
    co_return pool ? std::nullopt : std::optional{ pool.error() };
}

/**
 * @brief Run DDL, transactional writes, typed reads, NULL handling, and move-only values through Execute.
 */
auto soci_round_trip() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start SOCI round-trip runtime");
    auto pool = mhttp::db::SessionPool::Open(soci::sqlite3, ":memory:", { .size = 1 });
    if (!Check(pool.has_value(), "open in-memory SQLite pool")) { return; }
    CheckEq(pool->Size(), std::size_t{ 1 }, "pool reports its configured size");
    mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        sql << "create table person (id integer primary key, name text not null, weight real)";
    }));
    std::vector<std::string> names{ "Ada", "Grace", "你好 ' quote" };
    auto inserted = mhttp::SyncWait(runtime, pool->Execute([names](soci::session& sql) {
        soci::transaction transaction{ sql };
        for (auto const& name : names) {
            sql << "insert into person (name, weight) values (:name, 60.5)", soci::use(name);
        }
        transaction.commit();
        return names.size();
    }));
    CheckEq(inserted, names.size(), "value result returns from the worker thread");
    auto owned = std::make_unique<std::string>("Grace");
    auto found = mhttp::SyncWait(runtime, pool->Execute([owned = std::move(owned)](soci::session& sql) {
        auto id = std::make_unique<long long>(-1);
        sql << "select id from person where name = :name", soci::use(*owned), soci::into(*id);
        return id;
    }));
    Check(found && *found == 2, "move-only function and result cross the worker boundary");
    auto rows = mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        sql << "insert into person (name, weight) values ('nobody', null)";
        std::vector<std::pair<std::string, std::optional<double>>> result;
        soci::rowset<soci::row> rows = (sql.prepare << "select name, weight from person order by id");
        for (auto const& row : rows) {
            auto weight = row.get_indicator(1) == soci::i_null ? std::nullopt : std::optional{ row.get<double>(1) };
            result.emplace_back(row.get<std::string>(0), weight);
        }
        return result;
    }));
    if (Check(rows.size() == 4, "rowset reads every committed row")) {
        CheckEq(rows[2].first, names[2], "UTF-8 text and quotes round-trip through bound parameters");
        Check(rows[0].second == 60.5 && !rows[3].second, "row indicators distinguish values from NULL");
    }
    auto nulls = mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        double weight{ -1 };
        soci::indicator indicator{ soci::i_ok };
        sql << "select weight from person where name = 'nobody'", soci::into(weight, indicator);
        std::optional<double> optional{ 1.0 };
        sql << "select weight from person where name = 'nobody'", soci::into(optional);
        return indicator == soci::i_null && !optional;
    }));
    Check(nulls, "indicators and std::optional both observe NULL");
}

/**
 * @brief Propagate backend and application exceptions while rolling back and returning the session.
 */
auto soci_errors() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start SOCI error runtime");
    auto pool = mhttp::db::SessionPool::Open(soci::sqlite3, ":memory:", { .size = 1, .acquire_timeout = 500ms });
    if (!Check(pool.has_value(), "open single-session pool")) { return; }
    mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        sql << "create table person (id integer primary key, name text not null)";
        sql << "insert into person (id, name) values (1, 'Ada')";
    }));
    try {
        mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
            sql << "insert into person (id, name) values (1, 'duplicate')";
        }));
        Check(false, "duplicate key must fail");
    } catch (soci::sqlite3_soci_error const& error) {
        CheckEq(error.get_backend_name(), std::string{ "sqlite3" }, "backend exception keeps its dynamic type");
        Check(error.get_error_category() == soci::soci_error::constraint_violation, "backend error keeps its category");
    }
    try {
        mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
            sql << "select name from no_such_table";
        }));
        Check(false, "unknown table must fail");
    } catch (soci::soci_error const& error) {
        Check(std::string_view{ error.what() }.contains("no_such_table"), "backend diagnostic reaches the awaiting Task");
    }
    try {
        mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
            soci::transaction transaction{ sql };
            sql << "insert into person (id, name) values (2, 'rolled back')";
            throw std::runtime_error{ "application failure" };
        }));
        Check(false, "application exception must propagate");
    } catch (std::runtime_error const& error) {
        CheckEq(std::string_view{ error.what() }, "application failure"sv, "application exception keeps its message");
    }
    auto count = mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        int count{ -1 };
        sql << "select count(*) from person", soci::into(count);
        return count;
    }));
    CheckEq(count, 1, "the only session is reusable and the failed transaction rolled back");
}

/**
 * @brief Validate options, refuse blocking setup on I/O threads, and keep sessions alive for issued Tasks.
 */
auto soci_options_and_lifetime() -> void {
    using mhttp::db::SessionPool;
    for (auto options : { mhttp::db::SessionPoolOptions{ .size = 0 },
                          mhttp::db::SessionPoolOptions{ .acquire_timeout = 0ms },
                          mhttp::db::SessionPoolOptions{ .acquire_timeout = -1ms },
                          mhttp::db::SessionPoolOptions{
                              .acquire_timeout = std::chrono::milliseconds{ std::numeric_limits<int>::max() } + 1ms } }) {
        auto invalid = SessionPool::Open(soci::sqlite3, ":memory:", options);
        Check(!invalid && invalid.error() == mhttp::Error::InvalidRequest, "reject invalid pool options before connecting");
    }
    try {
        static_cast<void>(SessionPool::Open(soci::sqlite3, "db=:memory: timeout=invalid"));
        Check(false, "invalid backend option must fail");
    } catch (soci::soci_error const& error) {
        Check(std::string_view{ error.what() }.contains("timeout"), "connection failure keeps the backend diagnostic");
    }
    mhttp::Runtime runtime;
    Check(runtime.Start(1).has_value(), "start SOCI lifetime runtime");
    CheckEq(mhttp::SyncWait(runtime, open_on_io_thread()), std::optional{ mhttp::Error::Busy }, "refuse blocking connection setup on an I/O thread");
    auto pool = SessionPool::Open(soci::sqlite3, ":memory:", { .size = 1 });
    if (!Check(pool.has_value(), "open shared lifetime pool")) { return; }
    auto copy = *pool;
    mhttp::SyncWait(runtime, copy.Execute([](soci::session& sql) { sql << "create table shared (value integer)"; }));
    auto shared = mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        sql << "insert into shared (value) values (7)";
        int value{};
        sql << "select value from shared", soci::into(value);
        return value;
    }));
    CheckEq(shared, 7, "copies share the same connected sessions");
    std::optional<SessionPool> owner{ std::move(*pool) };
    CheckEq(pool->Size(), std::size_t{ 0 }, "moved-from pool reports no sessions");
    expect_error([&] {
        mhttp::SyncWait(runtime, pool->Execute([](soci::session&) { Check(false, "moved-from pool must not run work"); }));
    }, mhttp::Error::NotRunning, "moved-from pool rejects work");
    auto task = owner->Execute([](soci::session& sql) {
        int count{ -1 };
        sql << "select count(*) from shared", soci::into(count);
        return count;
    });
    owner.reset();
    copy = std::move(*pool);
    CheckEq(mhttp::SyncWait(runtime, std::move(task)), 1, "issued Task retains sessions after every pool object is gone");
}

/**
 * @brief Bound the wait for a leased session and recover once it is returned.
 */
auto soci_acquire_timeout() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start SOCI timeout runtime");
    auto pool = mhttp::db::SessionPool::Open(soci::sqlite3, ":memory:", { .size = 1, .acquire_timeout = 50ms });
    if (!Check(pool.has_value(), "open single-session timeout pool")) { return; }
    std::binary_semaphore held{ 0 }, release{ 0 };
    std::jthread holder{ [&] {
        try {
            mhttp::SyncWait(runtime, pool->Execute([&](soci::session&) {
                held.release();
                release.acquire();
            }));
        } catch (std::exception const& error) {
            Check(false, std::format("session holder failed: {}", error.what()));
        }
    } };
    held.acquire();
    auto const started = std::chrono::steady_clock::now();
    expect_error([&] {
        mhttp::SyncWait(runtime, pool->Execute([](soci::session&) { Check(false, "timed-out work must not run"); }));
    }, mhttp::Error::AcquireTimeout, "all sessions leased past the deadline");
    Check(std::chrono::steady_clock::now() - started >= 40ms, "acquisition waits for the configured timeout");
    release.release();
    holder.join();
    auto value = mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        int value{};
        sql << "select 42", soci::into(value);
        return value;
    }));
    CheckEq(value, 42, "returned session serves the next acquisition");
}

/**
 * @brief Run concurrent writers over a shared file with each session used by one function at a time.
 */
auto soci_concurrency() -> void {
    TemporaryDatabase database;
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start SOCI concurrency runtime");
    auto pool = mhttp::db::SessionPool::Open(soci::sqlite3, database.ConnectString(), { .size = 2 });
    if (!Check(pool.has_value(), "open file-backed pool")) { return; }
    mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        sql << "create table entry (worker integer not null, round integer not null, primary key (worker, round))";
    }));
    constexpr int WORKERS{ 4 };
    constexpr int ROUNDS{ 8 };
    std::mutex mutex;
    std::set<soci::session*> active;
    std::size_t peak{};
    {
        std::vector<std::jthread> threads;
        for (int worker{}; worker < WORKERS; ++worker) {
            threads.emplace_back([&, worker] {
                for (int round{}; round < ROUNDS; ++round) {
                    try {
                        mhttp::SyncWait(runtime, pool->Execute([&, worker, round](soci::session& sql) {
                            {
                                std::scoped_lock lock{ mutex };
                                Check(active.insert(&sql).second, "concurrent work never shares a session");
                                peak = std::max(peak, active.size());
                            }
                            sql << "insert into entry (worker, round) values (:worker, :round)", soci::use(worker), soci::use(round);
                            std::this_thread::sleep_for(2ms);
                            std::scoped_lock lock{ mutex };
                            active.erase(&sql);
                        }));
                    } catch (std::exception const& error) {
                        Check(false, std::format("concurrent insert failed: {}", error.what()));
                    }
                }
            });
        }
    }
    auto count = mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        int count{};
        sql << "select count(*) from entry", soci::into(count);
        return count;
    }));
    CheckEq(count, WORKERS * ROUNDS, "every concurrent write is stored exactly once");
    Check(peak >= 1 && peak <= pool->Size(), "concurrent sessions stay within the pool size");
}

/**
 * @brief Serve database-backed HTTP resources from coroutine handlers without blocking I/O threads.
 */
auto soci_http_handlers() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start SOCI HTTP runtime");
    auto opened = mhttp::db::SessionPool::Open(soci::sqlite3, ":memory:", { .size = 1 });
    if (!Check(opened.has_value(), "open HTTP handler pool")) { return; }
    auto pool = std::move(*opened);
    mhttp::SyncWait(runtime, pool.Execute([](soci::session& sql) {
        sql << "create table note (id integer primary key, text text not null)";
    }));
    mhttp::server::HttpServer server{ runtime };
    Check(server.Post("/notes", [pool](mhttp::server::Request& request, mhttp::server::Response& response) -> mhttp::Task<void> {
        auto id = co_await pool.Execute([text = std::string{ request.Body() }](soci::session& sql) {
            Check(!mhttp::Runtime::IsIoThread(), "database work runs outside I/O threads");
            sql << "insert into note (text) values (:text)", soci::use(text);
            long long id{};
            Check(sql.get_last_insert_id("note", id), "SQLite reports the inserted row id");
            return id;
        });
        Check(mhttp::Runtime::IsIoThread(), "handler resumes on an I/O thread");
        Check(response.SetStatus(mhttp::Status::Created).has_value(), "database insert is 201");
        Check(response.Headers().Set("Location", std::format("/notes/{}", id)).has_value(), "publish created note location");
    }).has_value(), "register database insert route");
    Check(server.Get("/notes/:id", [pool](mhttp::server::Request& request, mhttp::server::Response& response) -> mhttp::Task<void> {
        auto id = request.PathParam<long long>("id");
        if (!id) {
            Check(response.SetError(mhttp::Status::BadRequest).has_value(), "reject non-numeric note id");
            co_return;
        }
        auto text = co_await pool.Execute([id = *id](soci::session& sql) -> std::optional<std::string> {
            std::string text;
            sql << "select text from note where id = :id", soci::use(id), soci::into(text);
            return sql.got_data() ? std::optional{ std::move(text) } : std::nullopt;
        });
        Check((text ? response.SetContent(*text, "text/plain") : response.SetError(mhttp::Status::NotFound)).has_value(), "respond with note or 404");
    }).has_value(), "register database query route");
    auto port = server.BindToAnyPort("127.0.0.1");
    Check(port && server.Start() && server.WaitUntilReady(), "start loopback database server");
    if (!port) { return; }
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port };
    auto created = mhttp::SyncWait(runtime, client.Post("/notes", "persisted 你好", "text/plain"));
    Check(created && created->Status() == mhttp::Status::Created && created->Header("Location") == "/notes/1", "POST stores a note");
    auto fetched = mhttp::SyncWait(runtime, client.Get("/notes/1"));
    Check(fetched && fetched->Status() == mhttp::Status::Ok && fetched->Body() == "persisted 你好", "GET reads the stored note");
    auto missing = mhttp::SyncWait(runtime, client.Get("/notes/2"));
    Check(missing && missing->Status() == mhttp::Status::NotFound, "absent row is 404");
    auto invalid = mhttp::SyncWait(runtime, client.Get("/notes/abc"));
    Check(invalid && invalid->Status() == mhttp::Status::BadRequest, "invalid id never reaches the database");
    Check(server.Stop().has_value(), "stop database server");
}

/**
 * @brief Let running database work finish after Runtime cancellation and return its session for reuse.
 */
auto soci_cancellation() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(1).has_value(), "start SOCI cancellation runtime");
    auto pool = mhttp::db::SessionPool::Open(soci::sqlite3, ":memory:", { .size = 1, .acquire_timeout = 500ms });
    if (!Check(pool.has_value(), "open cancellation pool")) { return; }
    std::binary_semaphore entered{ 0 }, release{ 0 };
    std::atomic<bool> canceled{};
    std::jthread waiter{ [&] {
        try {
            mhttp::SyncWait(runtime, pool->Execute([&](soci::session& sql) {
                entered.release();
                release.acquire();
                sql << "create table finished (value integer)";
            }));
        } catch (std::system_error const& error) {
            canceled = error.code() == mhttp::make_error_code(mhttp::Error::Canceled);
        }
    } };
    entered.acquire();
    runtime.Stop();
    release.release();
    waiter.join();
    Check(canceled, "Runtime stop reports cancellation after the running function returns");
    Check(runtime.Join().has_value(), "join after canceled database work");
    Check(runtime.Start(1).has_value(), "restart Runtime with the same pool");
    auto tables = mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        int count{ -1 };
        sql << "select count(*) from sqlite_master where name = 'finished'", soci::into(count);
        return count;
    }));
    CheckEq(tables, 1, "canceled work completed and returned its session");
}

auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "optional SOCI round trip", soci_round_trip },
        { "optional SOCI errors", soci_errors },
        { "optional SOCI options and lifetime", soci_options_and_lifetime },
        { "optional SOCI acquire timeout", soci_acquire_timeout },
        { "optional SOCI concurrency", soci_concurrency },
        { "optional SOCI HTTP handlers", soci_http_handlers },
        { "optional SOCI cancellation", soci_cancellation },
    });
}
