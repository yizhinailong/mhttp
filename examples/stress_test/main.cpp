/**
 * @file main.cpp
 * @brief Load an HTTP endpoint through bounded independent clients and report completed attempts.
 */
import std;
import mhttp;

namespace {

    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    constexpr std::size_t SUB_BUCKETS = 32;
    constexpr std::size_t LATENCY_BUCKETS = 64 * SUB_BUCKETS;

    /**
     * @brief Own immutable command-line inputs retained through every worker's completion.
     */
    struct Config {
        std::string url; ///< Absolute plain-HTTP destination.
        std::string target; ///< Parsed path and query, excluding fragments.
        mhttp::Method method{ mhttp::Method::Get }; ///< Validated request method.
        std::string body; ///< Copied into each outgoing request.
        std::string content_type{ "text/plain" }; ///< Representation type when a body is specified.
        mhttp::Headers headers; ///< Validated custom fields.
        std::size_t connections{ 16 }; ///< At most one request per independent client.
        std::size_t threads{ 2 }; ///< Shared Runtime I/O execution threads.
        std::chrono::seconds duration{ 10 }; ///< Submission window in time mode.
        std::uint64_t requests{}; ///< Positive global count selects count mode.
        std::chrono::milliseconds timeout{ 3'000 }; ///< Overall budget for each complete attempt.
        bool has_body{}; ///< Preserve an explicitly provided empty body.
    };

    /**
     * @brief Accumulate one worker's statistics without synchronization or unbounded latency samples.
     * @note Only the owning worker mutates this object; the coordinator reads it after joining.
     */
    struct Stats {
        std::uint64_t attempts{}; ///< Includes HTTP responses and transport failures.
        std::uint64_t responses{}; ///< Complete responses of every status.
        std::uint64_t http_failures{}; ///< Complete responses outside the 2xx range.
        std::uint64_t transport_failures{}; ///< Failed Send results.
        std::uint64_t bytes{}; ///< Decoded response body bytes, excluding protocol overhead.
        double latency_sum{}; ///< Sum in milliseconds over all completed attempts.
        double latency_min{ std::numeric_limits<double>::infinity() }; ///< Minimum in milliseconds.
        double latency_max{}; ///< Maximum in milliseconds.
        std::array<std::uint64_t, LATENCY_BUCKETS> latencies{}; ///< 32 sub-buckets per microsecond octave.
        std::map<unsigned, std::uint64_t> statuses; ///< HTTP status frequencies.
        std::map<mhttp::Error, std::uint64_t> errors; ///< Transport error frequencies.
        std::exception_ptr failure; ///< Unexpected worker exception, read after joining.
    };

    /**
     * @brief Explain bounded options without starting a runtime or making network requests.
     */
    auto print_usage() -> void {
        std::cout << "usage: stress_test http://host[:port]/path [options]\n"
                     "  --connections N   independent keep-alive clients, 1..256 (default 16)\n"
                     "  --threads N       Runtime I/O threads, 1..64 (default 2)\n"
                     "  --duration N      submission window in seconds, 1..3600 (default 10)\n"
                     "  --requests N      total attempts, 1..1000000000; excludes --duration\n"
                     "  --timeout-ms N    overall request timeout, 1..600000 (default 3000)\n"
                     "  --method METHOD   GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS\n"
                     "  --body TEXT       owned request body, at most 1 MiB\n"
                     "  --content-type T  body media type (default text/plain)\n"
                     "  --header 'K: V'    add a custom header; repeatable\n"
                     "  --help            show this usage\n";
    }

    /**
     * @brief Parse a complete unsigned decimal, rejecting signs, suffixes, overflow, and unsafe bounds.
     * @param value Borrowed argument.
     * @param maximum Inclusive upper limit.
     * @return Positive integer.
     */
    auto positive_number(std::string_view value, std::uint64_t maximum) -> std::uint64_t {
        std::uint64_t number{};
        auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || end != value.data() + value.size() || number == 0 || number > maximum) {
            throw std::runtime_error{ "invalid positive numeric option: " + std::string{ value } };
        }
        return number;
    }

    /**
     * @brief Turn recoverable setup failures into a visible executable diagnostic.
     * @param result Completed setup result.
     */
    auto require_success(mhttp::Result<void> result) -> void {
        if (!result) { throw std::runtime_error{ std::string{ mhttp::ToString(result.error()) } }; }
    }

    /**
     * @brief Parse and validate all inputs before starting threads or touching the network.
     * @param argc Argument count including the executable.
     * @param argv Borrowed command-line storage, copied into the result.
     * @return Owned configuration.
     */
    auto parse_config(int argc, char* argv[]) -> Config {
        if (argc < 2) { throw std::runtime_error{ "an absolute HTTP URL is required; use --help" }; }
        Config config;
        config.url = argv[1];
        auto url = mhttp::Url::Parse(config.url);
        if (!url || url->Scheme() != "http") { throw std::runtime_error{ "expected an absolute http:// URL" }; }
        config.target = url->Target();
        bool count_given{}, duration_given{};
        for (int index = 2; index < argc; ++index) {
            std::string_view const option{ argv[index] };
            if (++index == argc) { throw std::runtime_error{ "missing value for " + std::string{ option } }; }
            std::string_view const value{ argv[index] };
            if (option == "--connections") { config.connections = positive_number(value, 256); }
            else if (option == "--threads") { config.threads = positive_number(value, 64); }
            else if (option == "--duration") {
                config.duration = std::chrono::seconds{ positive_number(value, 3600) };
                duration_given = true;
            } else if (option == "--requests") {
                config.requests = positive_number(value, 1'000'000'000);
                count_given = true;
            } else if (option == "--timeout-ms") {
                config.timeout = std::chrono::milliseconds{ positive_number(value, 600'000) };
            } else if (option == "--method") {
                auto method = mhttp::ParseMethod(value);
                if (!method || (*method != mhttp::Method::Get && *method != mhttp::Method::Head
                    && *method != mhttp::Method::Post && *method != mhttp::Method::Put
                    && *method != mhttp::Method::Patch && *method != mhttp::Method::Delete
                    && *method != mhttp::Method::Options)) { throw std::runtime_error{ "unsupported request method" }; }
                config.method = *method;
            } else if (option == "--body") {
                if (value.size() > 1024 * 1024) { throw std::runtime_error{ "body exceeds 1 MiB" }; }
                config.body = value;
                config.has_body = true;
            } else if (option == "--content-type") { config.content_type = value; }
            else if (option == "--header") {
                auto const separator = value.find(':');
                if (separator == std::string_view::npos) { throw std::runtime_error{ "header requires Name: value" }; }
                auto field = value.substr(separator + 1);
                while (!field.empty() && (field.front() == ' ' || field.front() == '\t')) { field.remove_prefix(1); }
                require_success(config.headers.Add(value.substr(0, separator), field));
            } else { throw std::runtime_error{ "unknown option: " + std::string{ option } }; }
        }
        if (count_given && duration_given) { throw std::runtime_error{ "choose --requests or --duration" }; }
        mhttp::client::Request probe{ config.method, config.target };
        if (config.has_body) { require_success(probe.SetContent(config.body, config.content_type)); }
        return config;
    }

    /**
     * @brief Record all-attempt latency with bounded storage and at most 1/32 octave bucket width.
     * @param stats Borrowed worker-owned counters.
     * @param elapsed Nonnegative monotonic attempt duration, including connection setup and failures.
     */
    auto record_latency(Stats& stats, Clock::duration elapsed) -> void {
        auto const milliseconds = std::chrono::duration<double, std::milli>{ elapsed }.count();
        stats.latency_sum += milliseconds;
        stats.latency_min = std::min(stats.latency_min, milliseconds);
        stats.latency_max = std::max(stats.latency_max, milliseconds);
        auto const microseconds = static_cast<std::uint64_t>(std::max<std::int64_t>(1,
            std::chrono::ceil<std::chrono::microseconds>(elapsed).count()));
        auto const exponent = std::bit_width(microseconds) - 1;
        auto const remainder = microseconds - (std::uint64_t{ 1 } << exponent);
        auto const sub = exponent >= 5 ? remainder >> (exponent - 5) : remainder << (5 - exponent);
        ++stats.latencies[exponent * SUB_BUCKETS + sub];
    }

    /**
     * @brief Keep one exclusive client active while sharing only bounded submission accounting.
     * @param runtime Longer-lived I/O owner; this Task never blocks its execution threads.
     * @param config Immutable borrowed configuration retained until every coordinator joins.
     * @param stats Worker-owned counters, read only after joining.
     * @param next Shared ticket dispenser for exact global count mode.
     * @param abort Set after an unexpected worker or thread-creation failure.
     * @param deadline Common last-submission time in duration mode.
     */
    auto run_worker(
        mhttp::Runtime& runtime,
        Config const& config,
        Stats& stats,
        std::atomic<std::uint64_t>& next,
        std::atomic<bool>& abort,
        Clock::time_point deadline
    ) -> mhttp::Task<void> {
        mhttp::client::HttpClient client{ runtime, config.url,
            { .timeout_policy = mhttp::client::TimeoutPolicy::Overall, .timeout = config.timeout } };
        if (auto error = client.InitializationError()) { throw std::runtime_error{ std::string{ mhttp::ToString(*error) } }; }
        while (!abort.load(std::memory_order_relaxed)) {
            if (config.requests != 0) {
                if (next.fetch_add(1, std::memory_order_relaxed) >= config.requests) { break; }
            } else if (Clock::now() >= deadline) { break; }
            mhttp::client::Request request{ config.method, config.target };
            request.Headers() = config.headers;
            if (config.has_body) { require_success(request.SetContent(config.body, config.content_type)); }
            auto const started = Clock::now();
            auto response = co_await client.Send(std::move(request));
            record_latency(stats, Clock::now() - started);
            ++stats.attempts;
            if (!response) {
                ++stats.transport_failures;
                ++stats.errors[response.error()];
                continue;
            }
            ++stats.responses;
            stats.bytes += response->Body().size();
            auto const status = static_cast<unsigned>(response->Status());
            ++stats.statuses[status];
            if (status < 200 || status >= 300) { ++stats.http_failures; }
        }
    }

    /**
     * @brief Find a nearest-rank latency bucket's conservative upper bound.
     * @param stats Aggregate completed-attempt histogram.
     * @param percent Positive percentile in [1, 100].
     * @return Approximate upper bound in milliseconds, or zero without attempts.
     */
    auto percentile(Stats const& stats, unsigned percent) -> double {
        if (stats.attempts == 0) { return 0; }
        auto const rank = stats.attempts / 100 * percent + (stats.attempts % 100 * percent + 99) / 100;
        std::uint64_t count{};
        for (std::size_t index{}; index < stats.latencies.size(); ++index) {
            count += stats.latencies[index];
            if (count >= rank) {
                return std::ceil(std::ldexp(1.0 + static_cast<double>(index % SUB_BUCKETS + 1) / SUB_BUCKETS,
                    static_cast<int>(index / SUB_BUCKETS))) / 1000;
            }
        }
        return stats.latency_max;
    }

    /**
     * @brief Merge counters after all owning worker threads and coroutine frames have joined.
     * @param workers Completed per-client counters.
     * @return Combined report, rethrowing any unexpected worker failure.
     */
    auto aggregate(std::vector<Stats> const& workers) -> Stats {
        Stats total;
        for (auto const& stats : workers) {
            if (stats.failure) { std::rethrow_exception(stats.failure); }
            total.attempts += stats.attempts;
            total.responses += stats.responses;
            total.http_failures += stats.http_failures;
            total.transport_failures += stats.transport_failures;
            total.bytes += stats.bytes;
            total.latency_sum += stats.latency_sum;
            total.latency_min = std::min(total.latency_min, stats.latency_min);
            total.latency_max = std::max(total.latency_max, stats.latency_max);
            for (std::size_t index{}; index < LATENCY_BUCKETS; ++index) { total.latencies[index] += stats.latencies[index]; }
            for (auto const& [status, count] : stats.statuses) { total.statuses[status] += count; }
            for (auto const& [error, count] : stats.errors) { total.errors[error] += count; }
        }
        return total;
    }

    /**
     * @brief Report completed attempts separately from HTTP/transport failures and decoded body bytes.
     * @param stats Joined aggregate counters.
     * @param elapsed Wall duration including client setup and outstanding-request drain.
     */
    auto print_report(Stats const& stats, Clock::duration elapsed) -> void {
        auto const seconds = std::chrono::duration<double>{ elapsed }.count();
        std::cout << std::format("Requests: {}\nResponses: {}\nHTTP failures: {}\nTransport failures: {}\nBody bytes: {}\n",
            stats.attempts, stats.responses, stats.http_failures, stats.transport_failures, stats.bytes);
        std::cout << std::format("Elapsed: {:.3f} s\nRequests/sec: {:.2f}\nBody bytes/sec: {:.2f}\n",
            seconds, seconds > 0 ? stats.attempts / seconds : 0, seconds > 0 ? stats.bytes / seconds : 0);
        std::cout << std::format("Latency (all attempts, ms): avg={:.3f} min={:.3f} p50<={:.3f} p95<={:.3f} p99<={:.3f} max={:.3f}\n",
            stats.attempts ? stats.latency_sum / stats.attempts : 0, stats.attempts ? stats.latency_min : 0,
            percentile(stats, 50), percentile(stats, 95), percentile(stats, 99), stats.latency_max);
        for (auto const& [status, count] : stats.statuses) { std::cout << std::format("Status {}: {}\n", status, count); }
        for (auto const& [error, count] : stats.errors) { std::cout << std::format("Error {}: {}\n", mhttp::ToString(error), count); }
    }

} // namespace

/**
 * @brief Run load with one bounded non-I/O coordinator per client and asynchronous shared I/O threads.
 * @param argc Argument count.
 * @param argv Absolute HTTP URL and documented options.
 * @return Zero for all-2xx responses, two for HTTP/transport failures, or one for invalid setup/exceptions.
 */
auto main(int argc, char* argv[]) -> int {
    try {
        if (argc == 2 && std::string_view{ argv[1] } == "--help") { print_usage(); return 0; }
        auto const config = parse_config(argc, argv);
        mhttp::Runtime runtime;
        require_success(runtime.Start(config.threads));
        std::vector<Stats> statistics(config.connections);
        std::atomic<std::uint64_t> next{};
        std::atomic<bool> abort{};
        std::vector<std::jthread> workers;
        workers.reserve(config.connections);
        auto const started = Clock::now();
        auto const deadline = started + config.duration;
        try {
            for (std::size_t index{}; index < config.connections; ++index) {
                workers.emplace_back([&, index] {
                    try { mhttp::SyncWait(runtime, run_worker(runtime, config, statistics[index], next, abort, deadline)); }
                    catch (...) { statistics[index].failure = std::current_exception(); abort.store(true); }
                });
            }
        } catch (...) { abort.store(true); throw; }
        workers.clear();
        auto const elapsed = Clock::now() - started;
        auto const total = aggregate(statistics);
        print_report(total, elapsed);
        runtime.Stop();
        require_success(runtime.Join());
        return total.http_failures || total.transport_failures ? 2 : 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
