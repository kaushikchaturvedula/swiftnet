// Minimal SwiftNet server for apples-to-apples benchmarking.
//
// Deliberately bare: NO logging middleware, NO CORS, NO body-parsing middleware
// -- so a wrk run measures the framework's per-request cost (accept -> parse ->
// route -> serialize -> write) and nothing else. The example servers enable
// per-request logging, which allocates and does sink I/O on every request and
// would swamp the signal we are trying to isolate (coroutine-frame allocation).
//
// Routes mirror the standard framework micro-benchmarks:
//   GET /            -> plaintext "Hello, World!"   (TechEmpower "plaintext")
//   GET /json        -> {"message":"Hello, World!"} (TechEmpower "json")
//   GET /user/:id    -> small JSON echoing the path param (router + param + JSON)
//
// Usage: swiftnet_bench [port] [threads]   (defaults: 8080, hardware_concurrency)

#include "swiftnet.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>

using namespace swiftnet;

static SwiftNet *g_app = nullptr;
static void on_sigint(int) { if (g_app) g_app->close(); }

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

    std::cout << "swiftnet_bench listening on :" << port
              << " (threads=" << (threads ? std::to_string(threads) : std::string("auto"))
              << ", frame_pool="
#if SWIFTNET_USE_FRAME_POOL
              << "ON"
#else
              << "OFF"
#endif
              << ")" << std::endl;

    if (threads)
        vthread_scheduler::instance().start(threads);
    app.listen(port, [] {});
    return 0;
}
