/**
 * @file test_static.cpp
 * @brief Verify static mount points: files, directory redirects and listings, and path safety.
 */
#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import nlohmann.json;
import mhttp;

#include "support.hpp"

using namespace test_support;

namespace {

    /**
     * @brief Populate a site directory and a secret file outside of it.
     * @param dir Temporary root.
     */
    auto make_site(TempDir const& dir) -> void {
        dir.Write("site/hello.txt", "hello file");
        dir.Write("site/page.HTML", "<p>page</p>");
        dir.Write("site/docs/index.html", "<h1>docs</h1>");
        dir.Write("site/list/a.txt", "a");
        dir.Write("site/list/sub/b.txt", "b");
        dir.Write("secret.txt", "top secret");
    }

    auto test_files_and_redirects() -> void {
        TempDir dir;
        make_site(dir);
        TestServer srv{ [&](mhttp::HttpServer& server) { server.GetRouter().SetStaticMountPoint("/static", dir.path / "site"); } };
        Client client{ srv.Endpoint() };

        auto const file{ client.Get("/static/hello.txt") };
        check(file && file->result() == http::status::ok && file->body() == "hello file", "files under the mount must be served");
        check(file && (*file)[http::field::content_type] == "text/plain", "the MIME type must follow the extension");
        auto const upper{ client.Get("/static/page.HTML") };
        check(upper && (*upper)[http::field::content_type] == "text/html", "extensions must match case-insensitively");

        auto const index{ client.Get("/static/docs/") };
        check(index && index->body() == "<h1>docs</h1>", "a directory with index.html must serve it");
        auto const redirect{ client.Get("/static/docs?x=1") };
        check(redirect && redirect->result() == http::status::moved_permanently && (*redirect)[http::field::location] == "/static/docs/?x=1", "a directory without '/' must redirect and keep the query");
        auto const bare{ client.Get("/static") };
        check(bare && bare->result() == http::status::moved_permanently && (*bare)[http::field::location] == "/static/", "the bare mount point must redirect to its trailing-slash form");

        auto const head{ client.Send(make_request(http::verb::head, "/static/hello.txt")) };
        check(head && head->result() == http::status::ok && head->body().empty(), "HEAD must be served for static files");
        auto const file_slash{ client.Get("/static/hello.txt/") };
        check(file_slash && file_slash->result() == http::status::not_found, "a file with a trailing '/' must be 404");
        auto const missing{ client.Get("/static/nope.txt") };
        check(missing && missing->result() == http::status::not_found, "missing files must be 404");
    }

    auto test_path_safety() -> void {
        TempDir dir;
        make_site(dir);
        TestServer srv{ [&](mhttp::HttpServer& server) { server.GetRouter().SetStaticMountPoint("/static", dir.path / "site"); } };
        auto status{ [&](std::string_view target) {
            Client client{ srv.Endpoint() };
            auto const resp{ client.Get(target) };
            return resp ? resp->result() : http::status::unknown;
        } };
        check(status("/static/../secret.txt") == http::status::bad_request, "'..' above the mount must be rejected");
        check(status("/static/%2E%2E/secret.txt") == http::status::bad_request, "encoded '..' above the mount must be rejected");
        check(status("/static/docs%2F..%2F..%2Fsecret.txt") == http::status::bad_request, "%2F must not become a directory separator");
        check(status("/static/list/sub/../a.txt") == http::status::ok, "'..' inside the mount is allowed");
    }

    auto test_listings() -> void {
        TempDir dir;
        make_site(dir);
        TestServer srv{ [&](mhttp::HttpServer& server) {
            auto& router{ server.GetRouter() };
            router.SetStaticMountPoint("/json", dir.path / "site");
            mhttp::MountPointEntry html{ "/html", dir.path / "site" };
            html.SetDirectoryFormat(mhttp::MountPointEntry::DirFormat::Html);
            router.SetStaticMountPoint(std::move(html));
            mhttp::MountPointEntry closed{ "/closed", dir.path / "site" };
            closed.SetEnabledDirectory(false);
            router.SetStaticMountPoint(std::move(closed));
        } };
        Client client{ srv.Endpoint() };

        auto const json{ client.Get("/json/list/") };
        check(json && (*json)[http::field::content_type].starts_with("application/json"), "JSON listings must be JSON");
        if (json) {
            auto const listing = nlohmann::json::parse(json->body(), nullptr, false);
            check(listing.is_array() && listing.size() == 2, std::format("the JSON listing must be a flat array of entries, got {}", json->body()));
            bool saw_dir{ false };
            bool saw_file{ false };
            for (auto const& entry : listing) {
                saw_dir |= entry.value("filename", "") == "sub" && entry.value("is_dir", false);
                saw_file |= entry.value("filename", "") == "a.txt" && entry.value("filesize", 0) == 1;
            }
            check(saw_dir && saw_file, "the JSON listing must describe directories and files");
        }

        auto const html{ client.Get("/html/list/") };
        check(html && html->body().find("<a href=\"sub/\">sub/</a>") != std::string::npos && html->body().find("<a href=\"a.txt\">a.txt</a>") != std::string::npos, "HTML listings must link entries");
        check(html && html->body().find("<a href=\"../\">") != std::string::npos, "a nested HTML listing must link its parent");
        auto const html_root{ client.Get("/html/") };
        check(html_root && html_root->body().find("<a href=\"../\">") == std::string::npos, "the mount root listing must not link outside the mount");

        auto const closed_dir{ client.Get("/closed/list/") };
        check(closed_dir && closed_dir->result() == http::status::forbidden, "listings must be refused when disabled");
        auto const closed_no_slash{ client.Get("/closed/list") };
        check(closed_no_slash && closed_no_slash->result() == http::status::not_found, "with listings off and no index, a bare directory must be 404, not a redirect");
        auto const closed_index{ client.Get("/closed/docs/") };
        check(closed_index && closed_index->body() == "<h1>docs</h1>", "index documents still work with listings off");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("files and redirects", test_files_and_redirects);
    run_case("path safety", test_path_safety);
    run_case("directory listings", test_listings);
    return finish("test_static");
}
