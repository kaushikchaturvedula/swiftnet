#ifndef http_server_hpp
#define http_server_hpp

#include "../net/acceptor.hpp"
#include "../net/tcp_socket.hpp"
#include "../vthread_scheduler.hpp"
#include "../vthread.hpp"
#include "../ws/websocket.hpp"
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <atomic>

namespace swiftnet::http
{

    // Headers as a small vector of pairs: a request/response has only a handful,
    // so linear scan beats a red-black tree and avoids a node allocation per
    // header (a measurable per-request cost in profiling).
    using header_list = std::vector<std::pair<std::string, std::string>>;

    // Set (overwrite-or-append) a header by case-insensitive name.
    void set_header(header_list &h, std::string_view name, std::string value);

    struct request
    {
        std::string method;
        std::string path;
        header_list headers;
        std::string body;
    };

    struct response
    {
        int status{200};
        header_list headers;
        std::string body;

        std::string to_string() const;
    };

    class server
    {
    public:
        // Async handler: returns a coroutine that client_task co_awaits, so a
        // handler can suspend on I/O and unmount the virtual thread.
        using handler_t = std::function<vthread(const request &, response &)>;

        explicit server(uint16_t port = 8080, int backlog = 1024);
        ~server();

        void route(const std::string &method, const std::string &path, handler_t h);
        // Register a WebSocket session handler for `path`. A request to that path
        // with an `Upgrade: websocket` header is handshaked and handed to `h`.
        void ws_route(const std::string &path, ws::handler_t h);

        void start(std::size_t threads = std::thread::hardware_concurrency());
        void stop();

    private:
        struct route_key
        {
            std::string method;
            std::string path;
            bool operator<(const route_key &o) const noexcept
            {
                return method < o.method || (method == o.method && path < o.path);
            }
        };

        vthread client_task(net::tcp_socket sock);
        // Supervisor coroutine: re-runs the accept loop if it ever exits. Taken
        // by value so the handler is copied into the coroutine frame (a capturing
        // lambda coroutine would dangle once its closure temporary is destroyed).
        vthread acceptor_loop(std::function<void(net::tcp_socket)> handler);

        net::acceptor acceptor_;
        std::map<route_key, handler_t> routes_;
        std::map<std::string, ws::handler_t> ws_routes_;
        std::atomic<bool> running_{false};
        std::atomic<bool> acceptor_supervisor_running_{false}; // Prevent multiple supervisors
    };

} // namespace swiftnet::http

#endif
