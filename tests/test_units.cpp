#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "detail/mpsc_queue.hpp"
#include "detail/work_queue.hpp"
#include "detail/router.hpp"
#include "event_loop.hpp"
#include "swiftnet.hpp"
#include "ws/websocket.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <thread>
#include <vector>
#include <poll.h>
#include <unistd.h>

using namespace swiftnet;

TEST_CASE("mask_from_poll maps POLL* to the canonical event mask")
{
    CHECK(mask_from_poll(POLLIN) == READABLE);
    CHECK(mask_from_poll(POLLOUT) == WRITABLE);
    CHECK(mask_from_poll(POLLIN | POLLOUT) == (READABLE | WRITABLE));
    CHECK(mask_from_poll(0) == 0u);
}

TEST_CASE("mpsc_queue: FIFO for a single producer")
{
    detail::mpsc_queue<int> q;
    int out = -1;
    CHECK(q.pop(out) == false);
    for (int i = 0; i < 100; ++i)
        q.push(i);
    for (int i = 0; i < 100; ++i)
    {
        REQUIRE(q.pop(out));
        CHECK(out == i);
    }
    CHECK(q.pop(out) == false);
}

TEST_CASE("mpsc_queue: multi-producer delivers every item exactly once")
{
    detail::mpsc_queue<int> q;
    constexpr int P = 4, N = 2000;
    std::vector<std::thread> producers;
    for (int p = 0; p < P; ++p)
        producers.emplace_back([&q, p] {
            for (int i = 0; i < N; ++i)
                q.push(p * N + i);
        });
    for (auto &t : producers)
        t.join();

    std::vector<char> seen(P * N, 0);
    int out = 0, count = 0;
    while (q.pop(out))
    {
        REQUIRE(out >= 0);
        REQUIRE(out < P * N);
        CHECK(seen[out] == 0);
        seen[out] = 1;
        ++count;
    }
    CHECK(count == P * N);
}

TEST_CASE("work_queue: basic push/pop/steal/empty")
{
    detail::work_queue q;
    std::coroutine_handle<> h;
    CHECK(q.empty());
    CHECK(q.pop(h) == false);
    CHECK(q.steal(h) == false);

    auto noop = std::noop_coroutine();
    q.push(noop);
    CHECK(!q.empty());
    REQUIRE(q.pop(h));
    CHECK(h == noop);
    CHECK(q.empty());

    q.push(noop);
    REQUIRE(q.steal(h));
    CHECK(h == noop);
    CHECK(q.empty());
}

TEST_CASE("event_loop: readiness arm is one-shot and round-trips the token")
{
    event_loop el;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    const std::uint64_t tok = 0xCAFEBABEull;
    el.arm(fds[0], mask_from_poll(POLLIN), tok);

    io_event evs[4];
    CHECK(el.wait(evs, 4, 50) == 0); // nothing readable yet

    char c = 'x';
    REQUIRE(::write(fds[1], &c, 1) == 1);
    int n = el.wait(evs, 4, 1000);
    REQUIRE(n == 1);
    CHECK(evs[0].token == tok);
    CHECK((evs[0].mask & READABLE) != 0);

    // One-shot: the watch is consumed, so no refire even though it's readable.
    CHECK(el.wait(evs, 4, 100) == 0);

    REQUIRE(::read(fds[0], &c, 1) == 1);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("event_loop: one-shot timer fires once with its token")
{
    event_loop el;
    io_event evs[4];
    el.arm_timer(0x99ull, 30);
    auto t0 = std::chrono::steady_clock::now();
    int n = el.wait(evs, 4, 2000);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    REQUIRE(n == 1);
    CHECK(evs[0].token == 0x99ull);
    CHECK(ms >= 20);
    CHECK(el.wait(evs, 4, 80) == 0); // one-shot
}

TEST_CASE("event_loop: wake() unblocks a blocked wait()")
{
    event_loop el;
    io_event evs[4];
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        el.wake();
    });
    int n = el.wait(evs, 4, 5000); // would block 5s without wake()
    waker.join();
    CHECK(n == 0);
}

TEST_CASE("Request: query, headers, body and JSON parsing")
{
    http::request hr;
    hr.method = "POST";
    hr.path = "/search?q=hello&n=5";
    hr.headers.emplace_back("Content-Type", "application/json");
    hr.body = R"({"a":1,"b":"two"})";

    Request req(hr);
    CHECK(req.method() == "POST");
    CHECK(req.query("q") == "hello");
    CHECK(req.query("n") == "5");
    CHECK(req.query("missing") == "");
    CHECK(req.is_json());
    CHECK(req.body() == R"({"a":1,"b":"two"})");

    Json j = req.json();
    CHECK(j["a"] == 1);
    CHECK(j["b"] == "two");
}

TEST_CASE("Request: route params via set_param")
{
    http::request hr;
    hr.method = "GET";
    hr.path = "/user/42";
    Request req(hr);
    req.set_param("id", "42");
    CHECK(req.param("id") == "42");
    CHECK(req.param("nope") == "");
}

TEST_CASE("Router: static/param/wildcard precedence, backtracking, method")
{
    using swiftnet::detail::Router;
    Router r;
    r.add("GET", "/", 0);
    r.add("GET", "/user/:id", 1);
    r.add("GET", "/user/me", 2);   // static must beat param
    r.add("GET", "/files/*", 3);   // trailing wildcard
    r.add("POST", "/user/:id", 4); // method-specific

    Router::Params p;
    auto m = [&](const char *meth, const char *path) {
        p.clear();
        return r.match(meth, path, p);
    };

    CHECK(m("GET", "/") == 0);
    CHECK(m("GET", "/user/42") == 1);
    REQUIRE(p.size() == 1);
    CHECK(p[0].first == "id");
    CHECK(p[0].second == "42");
    CHECK(m("GET", "/user/me") == 2);          // static precedence over :id
    CHECK(m("GET", "/files/a/b.txt") == 3);    // wildcard matches the rest
    CHECK(m("POST", "/user/9") == 4);          // method dispatch
    CHECK(m("DELETE", "/user/9") == Router::npos);
    CHECK(m("GET", "/nope") == Router::npos);
}

TEST_CASE("WebSocket handshake accept-key matches the RFC 6455 vector")
{
    // RFC 6455 section 1.3 example: key -> accept.
    CHECK(ws::WebSocket::accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
