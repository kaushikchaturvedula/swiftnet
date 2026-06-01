// SwiftNet async-handler demo.
//
// Demonstrates the headline feature: a route handler that `co_await`s async work.
// While the await is pending the virtual thread is UNMOUNTED from its CPU core
// (the worker runs other requests); when the async op completes the reactor
// re-mounts the handler exactly where it left off. The async op here is a reactor
// timer (no OS thread), standing in for a real async database/IO call.

#include "swiftnet.hpp"
#include "io_awaitable.hpp" // async_sleep
#include <csignal>
#include <iostream>

using namespace swiftnet;

// Fake async "database lookup": suspends the virtual thread on a reactor timer
// for a few ms, then returns a JSON record.
static vthread_base<Json> async_database_lookup(std::string id)
{
    co_await async_sleep(5); // unmount here; remount when the timer fires
    Json record;
    record["id"] = id;
    record["name"] = "User " + id;
    record["source"] = "async db (reactor timer, no OS thread)";
    co_return record;
}

static SwiftNet *g_app = nullptr;
static void on_sigint(int) { if (g_app) g_app->close(); }

int main()
{
    SwiftNet app(8081);
    g_app = &app;
    std::signal(SIGINT, on_sigint);

    app.logger();

    app.get("/", [](Request &, Response &res) {
        res.html("<h1>SwiftNet async demo</h1>"
                 "<p>Try <a href='/user/42'>/user/42</a> (async DB lookup) "
                 "or <a href='/slow'>/slow</a> (two chained awaits).</p>");
    });

    // Flagship: the handler co_awaits an async operation. This is exactly the
    // README example -- it now compiles and runs on the real reactor.
    app.get("/user/:id", [](Request &req, Response &res) -> vthread {
        std::string id = req.param("id");
        Json user = co_await async_database_lookup(id); // unmount/remount
        res.json(user);
        co_return;
    });

    // Chain multiple awaits to show repeated unmount/remount in one request.
    app.get("/slow", [](Request &, Response &res) -> vthread {
        co_await async_sleep(20);
        co_await async_sleep(20);
        res.json(Json{{"status", "ok"}, {"waited_ms", 40}});
        co_return;
    });

    std::cout << "async demo listening on http://localhost:8081\n"
                 "  GET /          - info\n"
                 "  GET /user/:id  - async DB lookup (co_await inside handler)\n"
                 "  GET /slow      - two chained async waits\n"
                 "Press Ctrl+C to stop.\n";
    app.listen();
    return 0;
}
