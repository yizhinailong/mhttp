/**
 * @file main.cpp
 * @brief Serve bounded in-memory notes with asynchronous handlers and synchronized application state.
 */
import std;
import mhttp;

namespace {

    using namespace std::chrono_literals;

    /**
     * @brief Keep recoverable setup/response failures visible in this small executable.
     * @param result Operation result, consumed during this call.
     */
    auto require_success(mhttp::Result<void> result) -> void {
        if (!result) { throw std::runtime_error{ std::string{ mhttp::ToString(result.error()) } }; }
    }

    /**
     * @brief Own the bounded note collection shared by concurrent session callbacks.
     */
    struct Notes {
        std::mutex mutex; ///< Protects notes and ID allocation during short synchronous operations.
        std::map<std::uint64_t, std::string> values; ///< At most 128 notes, each at most 256 bytes.
        std::uint64_t next_id{ 1 }; ///< Never reuse a deleted ID during this process.
    };

    /**
     * @brief Write an owned plain-text representation after assigning its status.
     * @param response Borrowed current response.
     * @param status Final status.
     * @param body Owned representation; empty for a 204 response.
     */
    auto respond(
        mhttp::server::Response& response,
        mhttp::Status status,
        std::string body
    ) -> void {
        require_success(response.SetStatus(status));
        require_success(response.SetContent(std::move(body), "text/plain"));
    }

    /**
     * @brief Keep list rows unambiguous and bound each stored representation.
     * @param body Borrowed request content.
     * @return Whether the text is nonempty, at most 256 bytes, and free of control bytes.
     */
    auto valid_text(std::string_view body) -> bool {
        return !body.empty() && body.size() <= 256 && std::ranges::none_of(body, [](unsigned char byte) {
            return byte < 0x20 || byte == 0x7f;
        });
    }

    /**
     * @brief Install handlers whose captured shared state outlives every admitted request.
     * @param server Idle listener that owns the registered callbacks.
     */
    auto install_routes(mhttp::server::HttpServer& server) -> void {
        auto notes = std::make_shared<Notes>();
        require_success(server.Get("/health", [](auto&, auto& response) -> mhttp::Task<void> {
            co_await mhttp::Sleep(1ms);
            require_success(response.SetContent("ready\n", "text/plain"));
        }));
        require_success(server.Get("/notes", [notes](auto& request, auto& response) {
            auto const limit = request.Query().template Get<unsigned>("limit").value_or(
                request.Query().Has("limit") ? 0U : 10U);
            if (limit == 0 || limit > 100 || request.Query().Count("limit") > 1) {
                respond(response, mhttp::Status::BadRequest, "limit must be an integer in [1, 100]\n");
                return;
            }
            std::string body;
            {
                std::scoped_lock lock{ notes->mutex };
                for (auto const& [id, text] : notes->values | std::views::take(limit)) {
                    body += std::format("{}\t{}\n", id, text);
                }
            }
            respond(response, mhttp::Status::Ok, std::move(body));
        }));
        require_success(server.Post("/notes", [notes](auto& request, auto& response) {
            if (!valid_text(request.Body())) {
                respond(response, mhttp::Status::BadRequest, "send 1-256 text bytes without control characters\n");
                return;
            }
            std::uint64_t id{};
            {
                std::scoped_lock lock{ notes->mutex };
                if (notes->values.size() < 128) {
                    id = notes->next_id++;
                    notes->values.emplace(id, request.Body());
                }
            }
            if (id == 0) {
                respond(response, mhttp::Status::Conflict, "note capacity reached\n");
                return;
            }
            require_success(response.Headers().Set("Location", std::format("/notes/{}", id)));
            respond(response, mhttp::Status::Created, request.Body());
        }));
        require_success(server.Get("/notes/:id", [notes](auto& request, auto& response) {
            auto const id = request.template PathParam<std::uint64_t>("id");
            if (!id || *id == 0) {
                respond(response, mhttp::Status::BadRequest, "invalid note ID\n");
                return;
            }
            std::optional<std::string> text;
            {
                std::scoped_lock lock{ notes->mutex };
                if (auto found = notes->values.find(*id); found != notes->values.end()) { text = found->second; }
            }
            respond(response, text ? mhttp::Status::Ok : mhttp::Status::NotFound, text.value_or("note not found\n"));
        }));
        auto update = [notes](auto& request, auto& response) {
            auto const id = request.template PathParam<std::uint64_t>("id");
            if (!id || *id == 0 || !valid_text(request.Body())) {
                respond(response, mhttp::Status::BadRequest, "invalid note ID or text\n");
                return;
            }
            bool found{};
            {
                std::scoped_lock lock{ notes->mutex };
                if (auto item = notes->values.find(*id); item != notes->values.end()) {
                    item->second = request.Body();
                    found = true;
                }
            }
            respond(response, found ? mhttp::Status::Ok : mhttp::Status::NotFound,
                found ? request.Body() : "note not found\n");
        };
        require_success(server.Put("/notes/:id", update));
        require_success(server.Patch("/notes/:id", std::move(update)));
        require_success(server.Options("/notes/:id", [](auto&, auto& response) {
            require_success(response.Headers().Set("Allow", "GET, HEAD, PUT, PATCH, DELETE, OPTIONS"));
            respond(response, mhttp::Status::NoContent, "");
        }));
        require_success(server.Delete("/notes/:id", [notes](auto& request, auto& response) {
            auto const id = request.template PathParam<std::uint64_t>("id");
            if (!id || *id == 0) {
                respond(response, mhttp::Status::BadRequest, "invalid note ID\n");
                return;
            }
            bool erased{};
            {
                std::scoped_lock lock{ notes->mutex };
                erased = notes->values.erase(*id) != 0;
            }
            respond(response, erased ? mhttp::Status::NoContent : mhttp::Status::NotFound,
                erased ? "" : "note not found\n");
        }));
    }

} // namespace

/**
 * @brief Start a loopback listener, then drain it when the non-I/O console thread receives Enter or EOF.
 * @param argc Argument count.
 * @param argv Optional decimal port; zero selects an available dynamic port.
 * @return Zero after clean shutdown, otherwise one for invalid setup or an operation failure.
 */
auto main(int argc, char* argv[]) -> int {
    try {
        auto const port = argc == 1 ? std::optional<std::uint16_t>{ 8080 }
            : mhttp::utils::parse_integer<std::uint16_t>(argv[1]);
        if (argc > 2 || !port) { throw std::runtime_error{ "usage: rest_server [port: 0-65535]" }; }
        mhttp::Runtime runtime;
        require_success(runtime.Start(2));
        mhttp::server::HttpServer server{ runtime, { .read_timeout = 5s, .write_timeout = 5s,
            .body_bytes = 4096, .max_connections = 64 } };
        install_routes(server);
        auto bound = server.Bind("127.0.0.1", *port);
        if (!bound) { throw std::runtime_error{ std::string{ mhttp::ToString(bound.error()) } }; }
        require_success(server.Start());
        require_success(server.WaitUntilReady());
        std::cout << "Listening on http://127.0.0.1:" << *bound << '\n'
            << "Press Enter to stop.\n" << std::flush;
        std::string line;
        std::getline(std::cin, line);
        require_success(server.Stop());
        return 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
