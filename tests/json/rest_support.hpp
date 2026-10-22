/**
 * @file rest_support.hpp
 * @brief Provide an application-owned user resource fixture for REST and concurrent HTTP acceptance.
 * @note Include after std, mhttp.json, and test_support.hpp; this fixture is never part of the library.
 */
#pragma once

namespace mhttp::test {

    /**
     * @brief Own one complete application user value independently of request and store lifetimes.
     */
    struct User {
        std::uint64_t id{}; ///< Stable resource identifier.
        std::string name; ///< Owned display name.
        unsigned age{}; ///< Validated application field.
    };

    /**
     * @brief Serialize access to application data when callbacks run on multiple Runtime threads.
     */
    class UserStore {
    private:
        std::mutex m_mutex; ///< Protect identifiers and every collection access.
        std::map<std::uint64_t, User> m_users; ///< Ordered application resources.
        std::uint64_t m_next_id{ 1 }; ///< Monotonic identifier under the same mutex.

    public:
        /**
         * @brief Create one resource and return an independent owned snapshot.
         * @param name Owned validated name.
         * @param age Validated age.
         * @return Newly created value.
         */
        auto Create(std::string name, unsigned age) -> User {
            std::lock_guard lock{ m_mutex };
            User user{ m_next_id++, std::move(name), age };
            m_users.emplace(user.id, user);
            return user;
        }

        /**
         * @brief Read one snapshot without exposing a reference protected by a released mutex.
         * @param id Resource identifier.
         * @return Owned user, absent when deleted or unknown.
         */
        auto Read(std::uint64_t id) -> std::optional<User> {
            std::lock_guard lock{ m_mutex };
            auto found = m_users.find(id);
            if (found == m_users.end()) { return {}; }
            return found->second;
        }

        /**
         * @brief Apply validated replacement or partial fields as one synchronized application update.
         * @param id Resource identifier.
         * @param name Optional owned replacement name.
         * @param age Optional replacement age.
         * @return Updated snapshot, absent for an unknown identifier.
         */
        auto Update(
            std::uint64_t id,
            std::optional<std::string> name,
            std::optional<unsigned> age
        ) -> std::optional<User> {
            std::lock_guard lock{ m_mutex };
            auto found = m_users.find(id);
            if (found == m_users.end()) { return {}; }
            if (name) { found->second.name = std::move(*name); }
            if (age) { found->second.age = *age; }
            return found->second;
        }

        /**
         * @brief Remove one application resource under the store mutex.
         * @param id Resource identifier.
         * @return Whether a resource existed.
         */
        auto Erase(std::uint64_t id) -> bool {
            std::lock_guard lock{ m_mutex };
            return m_users.erase(id) != 0;
        }

        /**
         * @brief Copy a consistent ordered collection while keeping filtering/serialization outside the lock.
         * @return Owned ordered snapshots.
         */
        auto Snapshot() -> std::vector<User> {
            std::lock_guard lock{ m_mutex };
            std::vector<User> result;
            for (auto const& [id, user] : m_users) { result.push_back(user); }
            return result;
        }
    };

    /**
     * @brief Own validated update fields before the request JSON value is destroyed.
     */
    struct UserUpdate {
        std::optional<std::string> name; ///< Present for a replacement or named patch field.
        std::optional<unsigned> age; ///< Present for a replacement or age patch field.
    };

    /**
     * @brief Validate application schema without retaining references into a request JSON tree.
     * @param request Borrowed complete request.
     * @param partial Whether omitted fields are permitted.
     * @return Owned validated fields, absent for invalid syntax, media type, or schema.
     */
    auto parse_user_update(
        server::Request const& request,
        bool partial
    ) -> std::optional<UserUpdate> {
        auto value = request.Json();
        if (!value || !value->is_object()) { return {}; }
        UserUpdate update;
        if (auto name = value->find("name"); name != value->end()) {
            if (!name->is_string()) { return {}; }
            update.name = name->get<std::string>();
            if (update.name->empty() || update.name->size() > 100) { return {}; }
        }
        if (auto age = value->find("age"); age != value->end()) {
            if (!age->is_number_integer() || *age < 0 || *age > 150) { return {}; }
            update.age = age->get<unsigned>();
        }
        if (partial ? (!update.name && !update.age) : (!update.name || !update.age)) { return {}; }
        return update;
    }

    /**
     * @brief Encode a resource independently of request, store, and response ownership.
     * @param user Borrowed owned snapshot.
     * @return JSON representation.
     */
    auto user_json(User const& user) -> Json {
        return { { "id", user.id }, { "name", user.name }, { "age", user.age } };
    }

    /**
     * @brief Write an owned user representation or a resource-not-found error.
     * @param response Borrowed response.
     * @param user Optional owned resource snapshot.
     */
    auto write_user(server::Response& response, std::optional<User> const& user) -> void {
        if (!user) { Check(response.SetError(Status::NotFound).has_value(), "unknown user is 404"); return; }
        Check(response.SetJson(user_json(*user)).has_value(), "serialize user snapshot");
    }

    /**
     * @brief Register an application REST resource with owned handlers and synchronized business state.
     * @param server Listener configured before startup.
     * @param store Shared application owner, retained by every route callback.
     */
    auto register_user_routes(
        server::HttpServer& server,
        std::shared_ptr<UserStore> store
    ) -> void {
        Check(server.SetErrorHandler([](auto&, auto& response) {
            Check(response.SetJson(Json{ { "error", static_cast<unsigned>(response.Status()) } }).has_value(), "uniform application error envelope");
        }).has_value(), "register REST error representation");
        Check(server.set_http_handler<Method::Post>("/users", [store](auto& request, auto& response) {
            auto update = parse_user_update(request, false);
            if (!update) { Check(response.SetError(Status::BadRequest).has_value(), "validate new user"); return; }
            auto user = store->Create(std::move(*update->name), *update->age);
            Check(response.SetStatus(Status::Created).has_value(), "user creation is 201");
            Check(response.Headers().Set("Location", "/users/" + std::to_string(user.id)).has_value(), "publish created resource location");
            write_user(response, user);
        }).has_value(), "register user creation");
        Check(server.set_http_handler<Method::Get>("/users/:id", [store](auto& request, auto& response) {
            auto id = request.template PathParam<std::uint64_t>("id");
            if (!id || *id == 0) { Check(response.SetError(Status::BadRequest).has_value(), "validate typed user id"); return; }
            write_user(response, store->Read(*id));
        }).has_value(), "register user lookup");
        Check(server.set_http_handler<Method::Put, Method::Patch>("/users/:id", [store](auto& request, auto& response) {
            auto id = request.template PathParam<std::uint64_t>("id");
            auto update = parse_user_update(request, request.Method() == Method::Patch);
            if (!id || *id == 0 || !update) { Check(response.SetError(Status::BadRequest).has_value(), "validate typed id and update schema"); return; }
            write_user(response, store->Update(*id, std::move(update->name), update->age));
        }).has_value(), "register user replacement/patch");
        Check(server.set_http_handler<Method::Delete>("/users/:id", [store](auto& request, auto& response) {
            auto id = request.template PathParam<std::uint64_t>("id");
            if (!id || *id == 0) { Check(response.SetError(Status::BadRequest).has_value(), "validate deletion id"); return; }
            Check((store->Erase(*id) ? response.SetEmpty(Status::NoContent) : response.SetError(Status::NotFound)).has_value(), "delete resource or report missing user");
        }).has_value(), "register user deletion");
        Check(server.set_http_handler<Method::Get>("/users", [store](auto& request, auto& response) {
            auto page = request.Query().Has("page") ? request.Query().template Get<unsigned>("page") : std::optional{ 1U };
            auto size = request.Query().Has("size") ? request.Query().template Get<unsigned>("size") : std::optional{ 10U };
            if (!page || !size || *page == 0 || *page > 1000000 || *size == 0 || *size > 100) {
                Check(response.SetError(Status::BadRequest).has_value(), "validate pagination parameters"); return;
            }
            auto users = store->Snapshot();
            auto name = request.Query().Get("name");
            std::erase_if(users, [name](auto const& user) { return !user.name.contains(name); });
            auto items = Json::array();
            auto first = std::min(users.size(), static_cast<std::size_t>(*page - 1) * *size);
            auto last = std::min(users.size(), first + *size);
            for (auto index = first; index < last; ++index) { items.push_back(user_json(users[index])); }
            Check(response.SetJson(Json{ { "items", std::move(items) }, { "page", *page }, { "size", *size },
                { "total", users.size() } }).has_value(), "serialize filtered page from one store snapshot");
        }).has_value(), "register user collection pagination/filtering");
    }

    /**
     * @brief Start a configured application peer on a dynamic IPv4 loopback port.
     * @param server Listener retaining registered application handlers.
     * @return Chosen port or an exception making fixture startup failure explicit.
     */
    auto start_rest_peer(server::HttpServer& server) -> std::uint16_t {
        auto port = server.BindToAnyPort("127.0.0.1");
        if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start REST peer" }; }
        return *port;
    }

} // namespace mhttp::test
