#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "scope.hpp" // pulls in swiftnet.hpp

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace swiftnet;

namespace swiftnet
{
    // Test-only seam (granted via `friend struct PluginTestAccess;` in SwiftNet): resolve the
    // composed handler a plugin registered, by matching method+path through the real radix router.
    struct PluginTestAccess
    {
        static handler_t *resolve(SwiftNet &app, const std::string &method, const std::string &path)
        {
            detail::Router::Params params;
            std::size_t idx = app.router_.match(method, path, params);
            if (idx == detail::Router::npos)
                return nullptr;
            return &app.routes_[idx].handler;
        }
    };
}

static Request make_req(const std::string &method, const std::string &path)
{
    http::request hr;
    hr.method = method;
    hr.path = path;
    return Request(hr);
}

// Drive a route handler to completion. A lazily-started vthread with no engine completes as a no-op
// (final_transfer -> on_root_complete returns when t_engine_ == nullptr), so one resume() runs the
// whole composed chain + handler synchronously. No live server needed.
static Response run(handler_t &h, Request &req)
{
    Response res;
    vthread t = h(req, res);
    t.handle().resume();
    return res;
}
static bool has_header(const Response &res, const std::string &k, const std::string &v)
{
    for (auto &[hk, hv] : res.to_http_response().headers)
        if (hk == k && hv == v)
            return true;
    return false;
}

TEST_CASE("plugin (a): scoped middleware applies to its routes, not to routes outside the scope")
{
    SwiftNet app(8080);
    int scoped_hits = 0;
    app.plugin([&](Scope &s) {
        s.use([&](Request &, Response &, std::function<void()> next) { ++scoped_hits; next(); });
        s.get("/users", [](Request &, Response &res) { res.text("v1 users"); });
    }, {.prefix = "/v1"});
    app.get("/open", [](Request &, Response &res) { res.text("open"); }); // root route, no scope

    auto *scoped = PluginTestAccess::resolve(app, "GET", "/v1/users");
    auto *open = PluginTestAccess::resolve(app, "GET", "/open");
    REQUIRE(scoped);
    REQUIRE(open);

    Request r1 = make_req("GET", "/v1/users");
    CHECK(run(*scoped, r1).to_http_response().body == "v1 users");
    CHECK(scoped_hits == 1);

    Request r2 = make_req("GET", "/open");
    run(*open, r2);
    CHECK(scoped_hits == 1); // the scoped middleware did NOT run for the out-of-scope route
}

TEST_CASE("plugin: a scoped middleware that doesn't call next() short-circuits the handler")
{
    SwiftNet app(8080);
    bool handler_ran = false;
    app.plugin([&](Scope &s) {
        s.use([](Request &, Response &res, std::function<void()> /*next*/) { res.status(401).text("no"); });
        s.get("/secret", [&](Request &, Response &res) { handler_ran = true; res.text("secret"); });
    }, {.prefix = "/v1"});
    auto *h = PluginTestAccess::resolve(app, "GET", "/v1/secret");
    REQUIRE(h);
    Request r = make_req("GET", "/v1/secret");
    Response res = run(*h, r);
    CHECK(!handler_ran);
    CHECK(res.status() == 401);
}

TEST_CASE("plugin (b): inheritance + ordering (parent then child) + prefix nesting")
{
    SwiftNet app(8080);
    std::vector<std::string> order;
    app.plugin([&](Scope &v1) {
        v1.use([&](Request &, Response &, std::function<void()> next) { order.push_back("A"); next(); });
        v1.get("/users", [](Request &, Response &res) { res.text("u"); });
        v1.plugin([&](Scope &admin) {
            admin.use([&](Request &, Response &, std::function<void()> next) { order.push_back("B"); next(); });
            admin.get("/stats", [](Request &, Response &res) { res.text("s"); });
        }, {.prefix = "/admin"});
    }, {.prefix = "/v1"});

    // full nested path resolves; wrong (un-prefixed) paths do not
    CHECK(PluginTestAccess::resolve(app, "GET", "/v1/admin/stats"));
    CHECK(!PluginTestAccess::resolve(app, "GET", "/admin/stats"));
    CHECK(!PluginTestAccess::resolve(app, "GET", "/v1/stats"));

    order.clear();
    auto *stats = PluginTestAccess::resolve(app, "GET", "/v1/admin/stats");
    REQUIRE(stats);
    Request r = make_req("GET", "/v1/admin/stats");
    CHECK(run(*stats, r).to_http_response().body == "s");
    CHECK(order == std::vector<std::string>{"A", "B"}); // parent before child

    order.clear();
    auto *users = PluginTestAccess::resolve(app, "GET", "/v1/users");
    REQUIRE(users);
    Request r2 = make_req("GET", "/v1/users");
    run(*users, r2);
    CHECK(order == std::vector<std::string>{"A"}); // only the parent scope's middleware
}

TEST_CASE("plugin (c): decorator inherit + child override + sibling isolation + wrong-type miss")
{
    SwiftNet app(8080);
    int v1_k = 0, admin_k = 0;
    bool wrong_type_null = false;
    int v2_k = -1;
    bool v2_missing = false;

    app.plugin([&](Scope &v1) {
        v1.decorate<int>("k", 1);
        v1_k = *v1.get<int>("k");                              // own value
        wrong_type_null = (v1.get<std::string>("k") == nullptr); // wrong T -> nullptr
        v1.plugin([&](Scope &admin) {
            admin.decorate<int>("k", 2);                       // override
            admin_k = *admin.get<int>("k");                   // resolves to child's 2
            admin.get("/s", [](Request &, Response &res) { res.text("s"); });
        }, {.prefix = "/admin"});
    }, {.prefix = "/v1"});

    app.plugin([&](Scope &v2) {
        auto leaked = v2.get<int>("k"); // sibling: must NOT see v1's "k"
        v2_missing = (leaked == nullptr);
        v2_k = leaked ? *leaked : -1;
    }, {.prefix = "/v2"});

    CHECK(v1_k == 1);
    CHECK(admin_k == 2);          // child override
    CHECK(wrong_type_null);       // wrong-type lookup is a safe nullptr
    CHECK(v2_missing);            // sibling isolation: no leak
    CHECK(v2_k == -1);
}

TEST_CASE("plugin (c2): get<T> returns shared_ptr<const T> (read-only after registration)")
{
    SwiftNet app(8080);
    bool is_const = false;
    app.plugin([&](Scope &s) {
        auto p = s.get<int>("missing"); // type check works even on a miss
        is_const = std::is_same_v<decltype(p), std::shared_ptr<const int>>;
    }, {.prefix = "/c"});
    CHECK(is_const); // a handler capturing a decorator cannot mutate shared state across engines
}

TEST_CASE("plugin (d): the composed handler does NOT reference the (destroyed) Scope")
{
    SwiftNet app(8080);
    // The Scope is created and destroyed entirely inside app.plugin() (it is a private local of
    // SwiftNet::plugin, never exposed). So by the time we resolve+run the handler below, every Scope
    // is already gone. If the composed handler held a reference to the dead Scope, this would be UB.
    app.plugin([](Scope &s) {
        s.use([](Request &, Response &res, std::function<void()> next) { res.header("X-Scoped", "1"); next(); });
        s.get("/x", [](Request &, Response &res) { res.text("ok"); });
    }, {.prefix = "/p"});
    // (no Scope is kept alive anywhere — nothing to capture it into)

    auto *h = PluginTestAccess::resolve(app, "GET", "/p/x");
    REQUIRE(h);
    Request r = make_req("GET", "/p/x");
    Response res = run(*h, r);
    CHECK(res.to_http_response().body == "ok"); // user handler ran
    CHECK(has_header(res, "X-Scoped", "1"));    // scoped chain ran (captured by value, not via Scope)
}

// Lifetime-tracked decorator payload for test (e): counts live instances.
struct Tracked
{
    int v{};
    static inline int alive = 0;
    Tracked() { ++alive; }
    explicit Tracked(int x) : v(x) { ++alive; }
    Tracked(const Tracked &o) : v(o.v) { ++alive; }
    Tracked(Tracked &&o) noexcept : v(o.v) { ++alive; }
    ~Tracked() { --alive; }
};

TEST_CASE("plugin (e): a captured decorator keeps its object alive after the Scope is gone")
{
    SwiftNet app(8080);
    CHECK(Tracked::alive == 0);
    app.plugin([](Scope &s) {
        s.decorate<Tracked>("t", Tracked{42});
        auto t = s.get<Tracked>("t"); // shared_ptr<const Tracked>, co-owns the heap object
        s.get("/r", [t](Request &, Response &res) { res.text(std::to_string(t->v)); });
    }, {.prefix = "/d"});
    // The Scope (and its decorator store) are destroyed now. The object survives only because the
    // handler captured the shared_ptr returned by get<T>.
    CHECK(Tracked::alive == 1); // still alive, held by the captured decorator
    auto *h = PluginTestAccess::resolve(app, "GET", "/d/r");
    REQUIRE(h);
    Request r = make_req("GET", "/d/r");
    CHECK(run(*h, r).to_http_response().body == "42"); // and it's still usable
}

TEST_CASE("plugin (f): prefix normalization (trailing/leading/missing slashes -> canonical path)")
{
    SwiftNet app(8080);
    app.plugin([](Scope &s) { s.get("/x", [](Request &, Response &res) { res.text("x"); }); }, {.prefix = "/a/"});
    app.plugin([](Scope &s) { s.get("y", [](Request &, Response &res) { res.text("y"); }); }, {.prefix = "/b"});
    app.plugin([](Scope &s) { s.get("/z", [](Request &, Response &res) { res.text("z"); }); }, {.prefix = "c"});
    CHECK(PluginTestAccess::resolve(app, "GET", "/a/x"));
    CHECK(PluginTestAccess::resolve(app, "GET", "/b/y"));
    CHECK(PluginTestAccess::resolve(app, "GET", "/c/z"));
}
