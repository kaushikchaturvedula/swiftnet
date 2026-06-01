#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "swiftnet.hpp"
#include "io_awaitable.hpp" // async_sleep

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace swiftnet;

static constexpr uint16_t kPort = 18099;

// Minimal blocking HTTP client: connect, send `payload`, read until the server
// closes the connection (we always send Connection: close), return the response.
static std::string raw_request(const std::string &payload, int timeout_ms = 3000)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ::close(fd);
        return {};
    }
    ::send(fd, payload.data(), payload.size(), 0);

    std::string resp;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return resp;
}

static std::string http_get(const std::string &path)
{
    return raw_request("GET " + path + " HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
}

static bool wait_ready()
{
    for (int i = 0; i < 100; ++i) // up to ~5s
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bool ok = ::connect(fd, (sockaddr *)&addr, sizeof(addr)) == 0;
        ::close(fd);
        if (ok)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

TEST_CASE("GET / returns 200 with the body")
{
    auto r = http_get("/");
    CHECK(r.find("200") != std::string::npos);
    CHECK(r.find("hello swiftnet") != std::string::npos);
}

TEST_CASE("route params are extracted")
{
    auto r = http_get("/echo/42");
    CHECK(r.find("200") != std::string::npos);
    CHECK(r.find("id=42") != std::string::npos);
}

TEST_CASE("query string parsing")
{
    auto r = http_get("/q?name=ada");
    CHECK(r.find("hi ada") != std::string::npos);
}

TEST_CASE("unknown route yields 404")
{
    auto r = http_get("/does-not-exist");
    CHECK(r.find("404") != std::string::npos);
}

TEST_CASE("POST with JSON body is parsed and echoed")
{
    std::string body = R"({"name":"ada","n":7})";
    std::string req = "POST /body HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body;
    auto r = raw_request(req);
    CHECK(r.find("200") != std::string::npos);
    CHECK(r.find("\"name\":\"ada\"") != std::string::npos);
    CHECK(r.find("\"n\":7") != std::string::npos);
}

TEST_CASE("chunked request body is decoded")
{
    // "ada" + "bra" as two chunks.
    std::string req = "POST /len HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n"
                      "Connection: close\r\n\r\n"
                      "3\r\nada\r\n3\r\nbra\r\n0\r\n\r\n";
    auto r = raw_request(req);
    CHECK(r.find("200") != std::string::npos);
    CHECK(r.find("len=6") != std::string::npos); // "adabra"
}

TEST_CASE("async handler (co_await) returns correctly")
{
    auto r = http_get("/async/9");
    CHECK(r.find("200") != std::string::npos);
    CHECK(r.find("async 9") != std::string::npos);
}

TEST_CASE("async middleware co_awaits and passes through")
{
    auto r = http_get("/mw");
    CHECK(r.find("200") != std::string::npos);
    CHECK(r.find("handler ran") != std::string::npos);
}

TEST_CASE("async middleware can short-circuit (Response::end)")
{
    auto r = http_get("/blocked");
    CHECK(r.find("403") != std::string::npos);
    CHECK(r.find("blocked by middleware") != std::string::npos);
    CHECK(r.find("should not reach") == std::string::npos);
}

TEST_CASE("keep-alive: two pipelined requests on one connection")
{
    std::string req = "GET /echo/1 HTTP/1.1\r\nHost: t\r\n\r\n"
                      "GET /echo/2 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    auto r = raw_request(req);
    CHECK(r.find("id=1") != std::string::npos);
    CHECK(r.find("id=2") != std::string::npos);
}

// --- WebSocket helpers (manual framing over a persistent socket) ---
static int ws_connect()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    struct timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}
static std::string recv_n(int fd, size_t n)
{
    std::string out;
    char buf[1024];
    while (out.size() < n)
    {
        ssize_t r = ::recv(fd, buf, std::min(sizeof(buf), n - out.size()), 0);
        if (r <= 0)
            break;
        out.append(buf, (size_t)r);
    }
    return out;
}

TEST_CASE("WebSocket handshake + masked frame echo")
{
    int fd = ws_connect();
    REQUIRE(fd >= 0);

    const std::string key = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC example
    std::string hs = "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Key: " + key +
                     "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    ::send(fd, hs.data(), hs.size(), 0);

    // Read the 101 response headers (up to the blank line).
    std::string resp;
    char c;
    while (resp.find("\r\n\r\n") == std::string::npos && ::recv(fd, &c, 1, 0) == 1)
        resp.push_back(c);
    CHECK(resp.find("101") != std::string::npos);
    CHECK(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

    // Send a masked client text frame "hello".
    std::string msg = "hello";
    std::string frame;
    frame.push_back((char)0x81);                       // FIN + text
    frame.push_back((char)(0x80 | msg.size()));        // mask bit + len
    const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    for (int i = 0; i < 4; ++i)
        frame.push_back((char)mask[i]);
    for (size_t i = 0; i < msg.size(); ++i)
        frame.push_back((char)(msg[i] ^ mask[i % 4]));
    ::send(fd, frame.data(), frame.size(), 0);

    // Read the server echo frame (unmasked): byte0, byte1=len, payload.
    std::string hdr = recv_n(fd, 2);
    REQUIRE(hdr.size() == 2);
    CHECK((((unsigned char)hdr[0]) & 0x0F) == 0x1); // text opcode
    size_t len = (unsigned char)hdr[1] & 0x7F;
    std::string payload = recv_n(fd, len);
    CHECK(payload == "echo: hello");
    ::close(fd);
}

int main(int argc, char **argv)
{
    SwiftNet app(kPort);

    app.get("/", [](Request &, Response &res) { res.text("hello swiftnet"); });
    app.get("/echo/:id", [](Request &req, Response &res) { res.text("id=" + req.param("id")); });
    app.get("/q", [](Request &req, Response &res) { res.text("hi " + req.query("name")); });
    app.post("/body", [](Request &req, Response &res) { res.json(req.json()); });
    app.post("/len", [](Request &req, Response &res) { res.text("len=" + std::to_string(req.body().size())); });
    app.get("/async/:id", [](Request &req, Response &res) -> vthread {
        std::string id = req.param("id");
        co_await async_sleep(2);
        res.text("async " + id);
        co_return;
    });
    // Async middleware: co_awaits, and short-circuits /blocked with 403.
    app.use_async([](Request &req, Response &res) -> vthread {
        co_await async_sleep(1);
        if (req.path() == "/blocked")
        {
            res.status(403).text("blocked by middleware");
            res.end();
        }
        co_return;
    });
    app.get("/mw", [](Request &, Response &res) { res.text("handler ran"); });
    app.get("/blocked", [](Request &, Response &res) { res.text("should not reach"); });
    app.ws("/ws", [](ws::WebSocket socket) -> vthread {
        for (;;)
        {
            auto m = co_await socket.recv();
            if (m.closed)
                break;
            co_await socket.send_text("echo: " + m.data);
        }
        co_return;
    });

    std::thread server([&] { app.listen(); });

    if (!wait_ready())
    {
        app.close();
        server.join();
        std::fprintf(stderr, "server failed to start\n");
        return 2;
    }

    doctest::Context ctx(argc, argv);
    int res = ctx.run();

    app.close();
    server.join();
    return res;
}
