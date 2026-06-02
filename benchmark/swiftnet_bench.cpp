// Minimal SwiftNet server for apples-to-apples benchmarking.
//
// Deliberately bare: NO logging middleware, NO CORS, NO body-parsing middleware
// -- so a wrk run measures the framework's per-request cost (accept -> parse ->
// route -> serialize -> write) and nothing else. The example servers enable
// per-request logging, which allocates and does sink I/O on every request and
// would swamp the signal we are trying to isolate.
//
// Routes:
//   GET /            -> plaintext "Hello, World!"   (TechEmpower "plaintext")
//   GET /json        -> {"message":"Hello, World!"} (TechEmpower "json")
//   GET /user/:id    -> small JSON echoing the path param (router + param + JSON)
//   GET /compute?n=K -> CPU-BOUND: offloads ~K*1000 iterations of mixing work as a
//                       stealable compute task (co_await swiftnet::offload), then
//                       replies. Used for the work-stealing-valve experiment, where
//                       the SERVER (not loopback) is the bottleneck. See
//                       benchmark/valve_experiment.sh and BENCHMARKS.md.
//
// Usage: swiftnet_bench [port] [threads]   (defaults: 8080, hardware_concurrency)
// Valve env: SWIFTNET_STEAL=0|1  SWIFTNET_STEAL_THRESHOLD=<int>
// Stats env: SWIFTNET_BENCH_STATS=<path>  -> append "ms d0 d1 .. steals" every 100ms

#include "swiftnet.hpp"
#include "vthread_scheduler.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace swiftnet;

static SwiftNet *g_app = nullptr;
static void on_sigint(int) { if (g_app) g_app->close(); }

// Deterministic CPU work (FNV-style mixing); returns a value used in the response
// so the optimizer cannot elide the loop. ~K*1000 iterations.
static std::uint64_t cpu_work(long kilo_iters)
{
    std::uint64_t acc = 1469598103934665603ULL;
    long iters = kilo_iters * 1000;
    for (long i = 0; i < iters; ++i)
    {
        acc ^= static_cast<std::uint64_t>(i);
        acc *= 1099511628211ULL;
    }
    return acc;
}

int main(int argc, char **argv)
{
    std::uint16_t port = (argc > 1) ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 8080;
    std::size_t threads = (argc > 2) ? static_cast<std::size_t>(std::atoi(argv[2])) : 0;

    SwiftNet app(port);
    g_app = &app;
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    app.get("/", [](Request &, Response &res) {
        res.text("Hello, World!");
    });

    app.get("/json", [](Request &, Response &res) {
        Json j;
        j["message"] = "Hello, World!";
        res.json(j);
    });

    app.get("/user/:id", [](Request &req, Response &res) {
        Json j;
        j["id"] = req.param("id");
        j["name"] = "user";
        res.json(j);
    });

    // CPU-bound route: the heavy work is offloaded as a STEALABLE compute task so
    // the work-stealing valve can move it off a busy engine onto an idle one. With
    // the valve off, the owning engine runs it (blocking that engine's light
    // connections); with the valve on, an idle engine steals it.
    app.get("/compute", [](Request &req, Response &res) -> vthread {
        long n = 20000; // default ~20M iterations
        std::string q = req.query("n");
        if (!q.empty())
            n = std::atol(q.c_str());
        std::uint64_t result = 0;
        co_await swiftnet::offload([n, &result] { result = cpu_work(n); });
        Json j;
        j["acc"] = result;
        j["n"] = n;
        res.json(j);
        co_return;
    });

    // CPU-bound route that runs the work INLINE (no offload). The work executes
    // inside the pinned connection coroutine, so it blocks the owning engine and
    // CANNOT be stolen -- the valve cannot help this. Used to show that offload is
    // what makes the valve effective (compare /compute vs /compute_inline).
    app.get("/compute_inline", [](Request &req, Response &res) {
        long n = 20000;
        std::string q = req.query("n");
        if (!q.empty())
            n = std::atol(q.c_str());
        Json j;
        j["acc"] = cpu_work(n); // blocks this engine; not stealable
        j["n"] = n;
        res.json(j);
    });

    // Start the scheduler first so the env-driven valve config is parsed before we
    // print the banner (and so app.listen()'s start() below is a no-op).
    vthread_scheduler::instance().start(threads ? threads : std::thread::hardware_concurrency());

    std::cout << "swiftnet_bench listening on :" << port
              << " (threads=" << (threads ? std::to_string(threads) : std::string("auto"))
              << ", steal=" << (vthread_scheduler::instance().steal_enabled() ? "ON" : "OFF")
              << ", frame_pool="
#if SWIFTNET_USE_FRAME_POOL
              << "ON"
#else
              << "OFF"
#endif
              << ")" << std::endl;

    // Optional: sample per-engine compute backlog + cumulative steal count over
    // time, so the valve experiment can plot queue-depth-over-time and steals.
    std::atomic<bool> sampling{true};
    std::thread sampler;
    if (const char *path = std::getenv("SWIFTNET_BENCH_STATS"))
    {
        std::string out = path;
        sampler = std::thread([out, &sampling] {
            std::ofstream f(out, std::ios::trunc);
            auto t0 = std::chrono::steady_clock::now();
            while (sampling.load(std::memory_order_relaxed))
            {
                auto s = vthread_scheduler::instance().get_stats();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0).count();
                f << ms;
                for (auto d : s.per_core_compute_depth)
                    f << ' ' << d;
                f << " steals=" << s.work_stolen << '\n';
                f.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    app.listen(port, [] {});

    sampling.store(false, std::memory_order_relaxed);
    if (sampler.joinable())
        sampler.join();
    return 0;
}
