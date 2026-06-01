#include "http/http_server.hpp"
#include "io_awaitable.hpp"
#include "detail/log.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>

using namespace swiftnet;
using namespace swiftnet::http;

namespace
{
    constexpr std::size_t kMaxHeaderBytes = 64 * 1024;       // 64 KiB of headers
    constexpr std::size_t kMaxBodyBytes = 8 * 1024 * 1024;   // 8 MiB body cap
    constexpr std::size_t kMaxRequestBytes = kMaxHeaderBytes + kMaxBodyBytes;

    bool iequals(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
               });
    }

    // Case-insensitive header lookup (HTTP header names are case-insensitive).
    const std::string *find_header(const std::map<std::string, std::string> &headers,
                                   std::string_view name)
    {
        for (const auto &[k, v] : headers)
            if (iequals(k, name))
                return &v;
        return nullptr;
    }

    // Decode a Transfer-Encoding: chunked body beginning at `start`.
    // Returns 1 complete (body filled, consumed = end offset), 0 need-more, -1 error.
    int decode_chunked(const std::string &data, std::size_t start,
                       std::string &body, std::size_t &consumed)
    {
        std::size_t pos = start;
        body.clear();
        while (true)
        {
            auto line_end = data.find("\r\n", pos);
            if (line_end == std::string::npos)
                return 0; // need the chunk-size line
            std::string size_str = data.substr(pos, line_end - pos);
            if (auto semi = size_str.find(';'); semi != std::string::npos)
                size_str = size_str.substr(0, semi); // drop chunk extensions
            std::size_t chunk_size = 0;
            try { chunk_size = std::stoul(size_str, nullptr, 16); }
            catch (...) { return -1; }

            std::size_t data_start = line_end + 2;
            if (chunk_size == 0)
            {
                // Final chunk: consume the terminating CRLF (trailers ignored).
                auto term = data.find("\r\n", data_start);
                if (term == std::string::npos)
                    return 0;
                consumed = term + 2;
                return 1;
            }
            if (body.size() + chunk_size > kMaxBodyBytes)
                return -1;
            if (data.size() < data_start + chunk_size + 2)
                return 0; // need chunk data + trailing CRLF
            body.append(data, data_start, chunk_size);
            pos = data_start + chunk_size + 2;
        }
    }
} // namespace

// Parse one HTTP/1.1 request (headers + body) from the front of `data`.
// Returns 1 = complete (req filled, consumed = total bytes), 0 = need more data,
// -1 = malformed / over limit.
static int parse_request(const std::string &data, request &req, std::size_t &consumed)
{
    auto pos_end = data.find("\r\n\r\n");
    if (pos_end == std::string::npos)
        return data.size() > kMaxHeaderBytes ? -1 : 0; // headers incomplete / too large
    std::size_t header_len = pos_end + 4;

    req.headers.clear();
    std::istringstream iss(data.substr(0, header_len));
    std::string line;
    if (!std::getline(iss, line))
        return -1;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    {
        std::istringstream l0(line);
        if (!(l0 >> req.method >> req.path))
            return -1;
    }
    while (std::getline(iss, line))
    {
        if (line == "\r" || line.empty() || line == "\n")
            break;
        if (line.back() == '\r')
            line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
            value.erase(0, 1);
        req.headers[std::move(key)] = std::move(value);
    }

    // Body framing: chunked takes precedence over Content-Length.
    req.body.clear();
    if (const std::string *te = find_header(req.headers, "transfer-encoding");
        te && te->find("chunked") != std::string::npos)
    {
        std::size_t body_consumed = 0;
        int st = decode_chunked(data, header_len, req.body, body_consumed);
        if (st <= 0)
            return st;
        consumed = body_consumed;
        return 1;
    }

    if (const std::string *cl = find_header(req.headers, "content-length"))
    {
        std::size_t n = 0;
        try { n = std::stoul(*cl); }
        catch (...) { return -1; }
        if (n > kMaxBodyBytes)
            return -1;
        if (data.size() < header_len + n)
            return 0; // need more body bytes
        req.body = data.substr(header_len, n);
        consumed = header_len + n;
        return 1;
    }

    consumed = header_len; // no body
    return 1;
}

std::string response::to_string() const
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " OK\r\n";
    if (headers.find("Content-Length") == headers.end())
        oss << "Content-Length: " << body.size() << "\r\n";
    for (const auto &[k, v] : headers)
        oss << k << ": " << v << "\r\n";
    oss << "\r\n";
    oss << body;
    return oss.str();
}

server::server(uint16_t port, int backlog) : acceptor_(port, backlog)
{
    SWIFTNET_LOG_DEBUG("http::server constructed (port={}, backlog={})", port, backlog);
}

server::~server() { stop(); }

void server::route(const std::string &method, const std::string &path, handler_t h)
{
    routes_.insert_or_assign(route_key{method, path}, std::move(h));
}

void server::ws_route(const std::string &path, ws::handler_t h)
{
    ws_routes_.insert_or_assign(path, std::move(h));
}

void server::start(std::size_t threads)
{
    if (running_)
        return;
    running_ = true;

    // The scheduler owns the I/O reactor; starting it brings the event loop online.
    vthread_scheduler::instance().start(threads);

    // Ensure only one acceptor supervisor runs.
    bool expected = false;
    if (!acceptor_supervisor_running_.compare_exchange_strong(expected, true))
        return;

    auto handler = [this](net::tcp_socket sock) {
        vthread_scheduler::instance().schedule(client_task(std::move(sock)));
    };
    vthread_scheduler::instance().schedule(acceptor_loop(std::move(handler)));

    SWIFTNET_LOG_INFO("http::server started with {} worker threads", threads);
}

vthread server::acceptor_loop(std::function<void(net::tcp_socket)> handler)
{
    SWIFTNET_LOG_DEBUG("acceptor supervisor started");
    while (running_)
    {
        try
        {
            co_await acceptor_.async_accept(handler);
        }
        catch (const std::exception &e)
        {
            SWIFTNET_LOG_ERROR("acceptor exception: {}, restarting after delay", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    acceptor_supervisor_running_ = false;
    co_return;
}

void server::stop()
{
    running_ = false;
}

vthread server::client_task(net::tcp_socket sock)
{
    std::array<char, 8192> buf;
    std::string accum;

    while (true)
    {
        request req;
        std::size_t consumed = 0;
        int st = parse_request(accum, req, consumed);

        if (st == 0)
        {
            // Need more bytes to complete headers and/or body.
            int n = co_await sock.async_read(buf.data(), buf.size());
            if (n <= 0)
            {
                sock.close();
                co_return;
            }
            accum.append(buf.data(), static_cast<std::size_t>(n));
            if (accum.size() > kMaxRequestBytes)
            {
                sock.close(); // oversized request: drop the connection
                co_return;
            }
            continue;
        }

        if (st < 0)
        {
            // Malformed request: best-effort 400 then close.
            response res;
            res.status = 400;
            res.body = "Bad Request";
            res.headers["Content-Type"] = "text/plain";
            res.headers["Connection"] = "close";
            std::string out = res.to_string();
            co_await sock.async_write(out.data(), out.size());
            sock.close();
            co_return;
        }

        // st == 1: a complete request is buffered.
        accum.erase(0, consumed);

        // WebSocket upgrade? Handshake, then hand the raw connection (plus any
        // already-buffered bytes) to the registered session handler.
        if (const std::string *upg = find_header(req.headers, "upgrade");
            upg && iequals(*upg, "websocket") && !ws_routes_.empty())
        {
            std::string ws_path = req.path;
            if (auto qp = ws_path.find('?'); qp != std::string::npos)
                ws_path = ws_path.substr(0, qp);
            auto wit = ws_routes_.find(ws_path);
            const std::string *key = find_header(req.headers, "sec-websocket-key");
            if (wit != ws_routes_.end() && key)
            {
                std::string accept = ws::WebSocket::accept_key(*key);
                std::string resp =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
                int w = co_await sock.async_write(resp.data(), resp.size());
                if (w < 0)
                {
                    sock.close();
                    co_return;
                }
                co_await wit->second(ws::WebSocket(std::move(sock), std::move(accum)));
                co_return; // connection now owned by the WebSocket session
            }
            // Upgrade requested but no matching ws route: fall through to HTTP.
        }

        // HTTP/1.1 keeps the connection alive unless the client says otherwise.
        const std::string *conn = find_header(req.headers, "connection");
        bool client_keep = !(conn && iequals(*conn, "close"));

        response res;
        auto it = routes_.find(route_key{req.method, req.path});
        if (it != routes_.end())
        {
            co_await it->second(req, res);
        }
        else
        {
            auto catch_all_it = routes_.find(route_key{"*", "*"});
            if (catch_all_it != routes_.end())
                co_await catch_all_it->second(req, res);
            else
            {
                res.status = 404;
                res.body = "Not Found";
                res.headers["Content-Type"] = "text/plain";
            }
        }

        res.headers["Connection"] = client_keep ? "keep-alive" : "close";
        std::string out = res.to_string();
        int w = co_await sock.async_write(out.data(), out.size());

        if (w < 0 || !client_keep)
        {
            sock.close();
            co_return;
        }
        // Otherwise loop: `accum` may already hold the next pipelined request.
    }
}
