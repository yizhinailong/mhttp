/**
 * @file soci_include_test.cpp
 * @brief Combine the SOCI headers with mhttp.soci for names the module does not re-export.
 * @note Textual SOCI includes come before every import, matching the documented consumer order.
 */
#include <soci/soci.h>

import std;
import mhttp.soci;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;

/**
 * @brief Read column metadata through header-only SOCI names inside pooled database work.
 */
auto soci_header_names() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(1).has_value(), "start SOCI header runtime");
    auto pool = mhttp::db::SessionPool::Open(soci::sqlite3, ":memory:", { .size = 1 });
    if (!Check(pool.has_value(), "open header consumer pool")) { return; }
    auto types = mhttp::SyncWait(runtime, pool->Execute([](soci::session& sql) {
        sql << "create table item (name text, price real)";
        sql << "insert into item (name, price) values ('tea', 2.5)";
        soci::row row;
        sql << "select name, price from item", soci::into(row);
        return std::pair{ row.get_properties(0).get_db_type(), row.get_properties(1).get_db_type() };
    }));
    Check(types.first == soci::db_string && types.second == soci::db_double, "header names and module names share one SOCI");
}

auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "optional SOCI header names", soci_header_names },
    });
}
