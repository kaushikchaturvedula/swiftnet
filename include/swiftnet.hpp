#ifndef SWIFTNET_HPP
#define SWIFTNET_HPP

#include "http/http_server.hpp"
#include "net/tcp_socket.hpp"
#include "vthread.hpp"
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <utility>

namespace swiftnet
{

    // Forward declarations
    class SwiftNet;
    class Request;
    class Response;

    // Type aliases
    using Json = nlohmann::json;
    using middleware_t = std::function<void(Request &, Response &, std::function<void()>)>;
    // Route handler: a coroutine that may co_await async I/O. Plain sync handlers
    // (void lambdas) are accepted too and adapted automatically (see
    // SwiftNet::get / make_handler), so both styles work:
    //   app.get("/a", [](Request&, Response& r){ r.text("hi"); });
    //   app.get("/b", [](Request&, Response& r) -> vthread { auto x = co_await ...; r.json(x); });
    using handler_t = std::function<vthread(Request &, Response &)>;
    // Async middleware: runs before routing, may co_await async I/O, and may
    // short-circuit by calling Response::end() (Express-style: ending the
    // response stops the chain before the handler). The synchronous middleware_t
    // above remains for non-suspending cross-cutting concerns (CORS, logging).
    using async_middleware_t = std::function<vthread(Request &, Response &)>;

    // Request class
    class Request
    {
    public:
        explicit Request(const http::request &req);

        const std::string &method() const { return method_; }
        const std::string &path() const { return path_; }
        const std::string &body() const { return body_; }
        const std::unordered_map<std::string, std::string> &headers() const { return headers_; }

        // Get header value
        std::string header(const std::string &name) const;

        // Get query parameter
        std::string query(const std::string &name) const;

        // Get route parameter (set by router)
        std::string param(const std::string &name) const;
        void set_param(const std::string &name, const std::string &value);

        // JSON parsing
        bool is_json() const;
        Json json() const;

        // Form data parsing
        std::unordered_map<std::string, std::string> form() const;

        // File operations
        bool has_file(const std::string &field) const;

    private:
        std::string method_;
        std::string path_;
        std::string body_;
        std::unordered_map<std::string, std::string> headers_;
        std::unordered_map<std::string, std::string> query_params_;
        std::unordered_map<std::string, std::string> route_params_;
        mutable Json json_cache_;
        mutable bool json_parsed_{false};

        void parse_query_string();
    };

    // Response class
    class Response
    {
    public:
        Response();

        // Status
        Response &status(int code);
        int status() const { return status_; }

        // Headers
        Response &header(const std::string &name, const std::string &value);
        Response &headers(const std::unordered_map<std::string, std::string> &headers);

        // Content
        Response &text(const std::string &content);
        Response &html(const std::string &content);
        Response &json(const Json &data);
        Response &file(const std::string &filepath);
        Response &send(const std::string &content);

        // Convenience methods
        Response &ok(const std::string &content = "");
        Response &created(const Json &data = Json{});
        Response &bad_request(const std::string &message = "Bad Request");
        Response &unauthorized(const std::string &message = "Unauthorized");
        Response &forbidden(const std::string &message = "Forbidden");
        Response &not_found(const std::string &message = "Not Found");
        Response &internal_error(const std::string &message = "Internal Server Error");

        // Redirect
        Response &redirect(const std::string &url, int code = 302);

        // Cookies
        Response &cookie(const std::string &name, const std::string &value,
                        const std::string &path = "/", int max_age = 0);

        // End the response now: async middleware calls this to short-circuit the
        // request (skip remaining middleware and the route handler).
        Response &end();
        bool ended() const { return ended_; }

        // Convert to HTTP response
        http::response to_http_response() const;

    private:
        int status_;
        std::unordered_map<std::string, std::string> headers_;
        std::string body_;
        bool ended_{false};
    };

    // Route structure
    struct Route
    {
        std::string method;
        std::string pattern;
        std::regex regex;
        std::vector<std::string> param_names;
        handler_t handler;
    };

    // SwiftNet class
    class SwiftNet
    {
    public:
        explicit SwiftNet(uint16_t port = 8080);
        ~SwiftNet();

        // HTTP methods. Accept either a sync handler `void(Request&,Response&)`
        // or an async coroutine handler `vthread(Request&,Response&)` that may
        // co_await async I/O; both are adapted to handler_t via make_handler.
        template <class F> SwiftNet &get(const std::string &path, F &&h) { return add_route("GET", path, make_handler(std::forward<F>(h))); }
        template <class F> SwiftNet &post(const std::string &path, F &&h) { return add_route("POST", path, make_handler(std::forward<F>(h))); }
        template <class F> SwiftNet &put(const std::string &path, F &&h) { return add_route("PUT", path, make_handler(std::forward<F>(h))); }
        template <class F> SwiftNet &del(const std::string &path, F &&h) { return add_route("DELETE", path, make_handler(std::forward<F>(h))); }
        template <class F> SwiftNet &patch(const std::string &path, F &&h) { return add_route("PATCH", path, make_handler(std::forward<F>(h))); }
        template <class F> SwiftNet &options(const std::string &path, F &&h) { return add_route("OPTIONS", path, make_handler(std::forward<F>(h))); }
        template <class F> SwiftNet &head(const std::string &path, F &&h) { return add_route("HEAD", path, make_handler(std::forward<F>(h))); }

        // Register a WebSocket session handler:
        //   app.ws("/chat", [](ws::WebSocket s) -> vthread {
        //       for (auto m = co_await s.recv(); !m.closed; m = co_await s.recv())
        //           co_await s.send_text(m.data);
        //   });
        SwiftNet &ws(const std::string &path, ws::handler_t handler);

        // Middleware
        SwiftNet &use(middleware_t middleware);
        SwiftNet &use(const std::string &path, middleware_t middleware);
        // Register async middleware (runs before routing; may co_await).
        SwiftNet &use_async(async_middleware_t middleware);

        // Static files
        SwiftNet &static_files(const std::string &path, const std::string &root);

        // Convenience middleware
        SwiftNet &cors(const std::string &origin = "*");
        SwiftNet &json(size_t limit = 1024 * 1024); // 1MB default
        SwiftNet &logger();

        // Server control
        void listen(std::function<void()> callback = nullptr);
        void listen(uint16_t port, std::function<void()> callback = nullptr);
        void close();

        // Configuration
        SwiftNet &set_threads(size_t threads);
        SwiftNet &set_backlog(int backlog);

    private:
        uint16_t port_;
        size_t threads_;
        int backlog_;
        bool running_;
        
        // Blocking mechanism for listen()
        std::condition_variable shutdown_cv_;
        std::mutex shutdown_mutex_;
        bool shutdown_requested_{false};

        std::vector<Route> routes_;
        std::vector<std::pair<std::string, ws::handler_t>> ws_routes_;
        std::vector<middleware_t> middlewares_;
        std::vector<std::pair<std::string, middleware_t>> path_middlewares_;
        std::vector<async_middleware_t> async_middlewares_;
        std::unique_ptr<http::server> server_;

        SwiftNet &add_route(const std::string &method, const std::string &path, handler_t handler);
        // Coroutine bridge: builds Request/Response, runs middleware, then
        // co_awaits the matched async handler. Awaited by the http catch-all.
        vthread handle_request_async(const http::request &req, http::response &res);
        bool match_route(const Route &route, const std::string &method,
                         const std::string &path, Request &request);
        Route create_route(const std::string &method, const std::string &pattern, handler_t handler);
        // Run the applicable middleware chain. Returns true if it fell through to
        // the route handler (no middleware short-circuited the request).
        bool run_middlewares(Request &req, Response &res);

        // Adapt a sync (void) or async (vthread) callable into handler_t.
        template <class F>
        static handler_t make_handler(F &&f)
        {
            using Ret = std::invoke_result_t<F &, Request &, Response &>;
            if constexpr (std::is_same_v<std::decay_t<Ret>, vthread>)
            {
                return handler_t(std::forward<F>(f)); // already an async coroutine handler
            }
            else
            {
                // Wrap a sync handler in a non-suspending coroutine.
                return [fn = std::forward<F>(f)](Request &req, Response &res) -> vthread
                {
                    fn(req, res);
                    co_return;
                };
            }
        }
    };

    // Utility functions
    namespace utils
    {
        std::string url_decode(const std::string &str);
        std::string url_encode(const std::string &str);
        std::unordered_map<std::string, std::string> parse_query_string(const std::string &query);
        std::string mime_type(const std::string &filepath);
        std::string read_file(const std::string &filepath);
        bool file_exists(const std::string &filepath);
        size_t file_size(const std::string &filepath);
        Json parse_json(const std::string &str);
        std::string serialize_json(const Json &json);
    }

    // WebSocket support (basic structure for future implementation)
    namespace ws
    {
        class WebSocket;
        using ws_handler_t = std::function<void(WebSocket &)>;
        using ws_message_handler_t = std::function<void(WebSocket &, const std::string &)>;
        using ws_close_handler_t = std::function<void(WebSocket &)>;

        class WebSocketServer
        {
        public:
            explicit WebSocketServer(SwiftNet &app);
            void on_connection(ws_handler_t handler);
            void on_message(ws_message_handler_t handler);
            void on_close(ws_close_handler_t handler);

        private:
            SwiftNet &app_;
            ws_handler_t connection_handler_;
            ws_message_handler_t message_handler_;
            ws_close_handler_t close_handler_;
        };
    }

    // Logger singleton
    class Logger
    {
    public:
        static Logger &instance();
        void info(const std::string &message);
        void warn(const std::string &message);
        void error(const std::string &message);
        void debug(const std::string &message);

    private:
        Logger();
        std::shared_ptr<spdlog::logger> logger_;
    };

    // MIME type mappings
    extern const std::unordered_map<std::string, std::string> MIME_TYPES;
}

#endif // SWIFTNET_HPP 