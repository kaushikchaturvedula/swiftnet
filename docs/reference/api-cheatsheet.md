# API cheat sheet

A one-screen reference for the most-used SwiftNet API. Every handler snippet assumes `using namespace swiftnet;`. Follow the links for the full guides.

## Quick start

```cpp
#include <swiftnet.hpp>
using namespace swiftnet;

struct User { int id{}; std::string name; bool active{}; };

int main() {
    SwiftNet app(8080);

    app.get("/health", [](Request&, Response& res) {
        res.text("ok");
    });

    app.get("/users/:id", [](Request& req, Response& res) {
        User u{ std::stoi(req.param("id")), "Ada", true };
        res.json(u); // typed serialize via Glaze
    });

    app.listen([]{ /* server is up */ });
}
```

## `SwiftNet`

The application object. Header: `include/swiftnet.hpp`.

| Member | Signature | Description |
| --- | --- | --- |
| ctor | `SwiftNet(uint16_t port = 8080)` | Create the app bound to `port`. |
| `get` / `post` / `put` / `del` / `patch` / `options` / `head` | `SwiftNet&(const std::string& path, F&& handler)` | Register a route. Returns `*this` for chaining. |
| `use` | `SwiftNet&(middleware_t)` | Global middleware (runs outermost). |
| `use` | `SwiftNet&(const std::string& pathPrefix, middleware_t)` | Middleware scoped to a path prefix. |
| `use_async` | `SwiftNet&(async_middleware_t)` | Async middleware; runs before routing, may `co_await`. |
| `ws` | `SwiftNet&(const std::string& path, ws::handler_t)` | Register a WebSocket handler. |
| `static_files` | `SwiftNet&(const std::string& mount, const std::string& root)` | Serve files from `root` under `mount`. |
| `cors` | `SwiftNet&(const std::string& origin = "*")` | Add a CORS middleware. |
| `json` | `SwiftNet&(size_t limit = 1024*1024)` | Add a JSON body middleware with a size limit. |
| `logger` | `SwiftNet&()` | Add a request-logging middleware. |
| `plugin` | `SwiftNet&(Fn&& fn, PluginOpts opts = {})` | Register an encapsulated scope. `PluginOpts{ .prefix = "/v1" }`. |
| `listen` | `void(std::function<void()> callback = nullptr)` | Start serving; blocks. Also `listen(uint16_t port, callback)`. |
| `close` | `void()` | Stop the server. |
| `set_threads` | `SwiftNet&(size_t threads)` | Override the engine count. |
| `set_backlog` | `SwiftNet&(int backlog)` | Override the accept backlog. |

> A handler is either a sync `void(Request&, Response&)` or an async `vthread(Request&, Response&)` coroutine. Both are accepted by every route verb and adapted automatically.

> Programmatic settings (port, engines, backlog) are the lowest-but-one layer of config; a `swiftnet.yaml` file overrides them, and environment variables always win. See the configuration guide for the full precedence and knob list.

## `Request`

Read-only view of the incoming request. Header: `include/swiftnet.hpp`.

| Member | Signature | Description |
| --- | --- | --- |
| `method` | `const std::string&()` | HTTP method, e.g. `"GET"`. |
| `path` | `const std::string&()` | Request path. |
| `header` | `std::string(const std::string& name)` | Header value (case-insensitive lookup), or empty. |
| `query` | `std::string(const std::string& name)` | Query-string parameter, or empty. |
| `param` | `std::string(const std::string& name)` | Route parameter from a `:name` segment, or empty. |
| `body` | `const std::string&()` | Raw request body. |
| `json` | `Json()` | Parse body as dynamic `Json` (`nlohmann::json`). |
| `body<T>` | `T()` | Typed parse via Glaze. Returns a default-constructed `T` on error (never throws). |
| `validate<T>` | `Validated<T>()` | Parse via `body<T>()`, then run `schema<T>`. |
| `bind<T>` | `std::optional<T>(Response& res)` | Parse + validate; on failure writes a 400 JSON to `res` and returns `nullopt`. |

## `Response`

Mutable outgoing response. All setters are chainable. Header: `include/swiftnet.hpp`.

| Member | Signature | Description |
| --- | --- | --- |
| `status` | `Response&(int code)` | Set the status code. |
| `text` | `Response&(const std::string& content)` | Plain-text body. |
| `json` | `Response&(const Json& data)` | Dynamic JSON body. |
| `json<T>` | `Response&(const T& value)` | Typed JSON via Glaze (when `T` is `GlazeSerializable`). |
| `header` | `Response&(const std::string& k, const std::string& v)` | Set a response header. |

Status helpers (each returns `Response&`):

| Helper | Status |
| --- | --- |
| `ok(content = "")` | 200 |
| `created(data = {})` | 201 |
| `bad_request(message = "Bad Request")` | 400 |
| `unauthorized(message = "Unauthorized")` | 401 |
| `forbidden(message = "Forbidden")` | 403 |
| `not_found(message = "Not Found")` | 404 |
| `internal_error(message = "Internal Server Error")` | 500 |

```cpp
app.get("/users/:id", [](Request& req, Response& res) {
    if (req.param("id").empty()) { res.bad_request("missing id"); return; }
    res.status(200).header("X-Source", "cache").json(Json{{"id", req.param("id")}});
});
```

## `Scope` (plugins)

Passed to your `plugin(fn, {prefix})` callback at registration time. Header: `include/scope.hpp`. Scope route verbs mirror `SwiftNet` and return `Scope&`.

| Member | Signature | Description |
| --- | --- | --- |
| `get` / `post` / `put` / `del` / `patch` / `options` / `head` | `Scope&(const std::string& path, F&& handler)` | Register a route under the scope prefix. |
| `use` | `Scope&(middleware_t)` | Middleware for this scope and its descendants (not siblings). |
| `plugin` | `Scope&(Fn&& fn, PluginOpts opts = {})` | Nest a child scope. |
| `decorate<T>` | `Scope&(const std::string& key, T value)` | Attach typed state (inherited by children, overridable). |
| `get<T>` | `std::shared_ptr<const T>(const std::string& key) const` | Read decorated state (read-only). Missing or wrong-type key returns `nullptr`. |

> Encapsulation is resolved at registration: scoped middleware is composed into each handler, so there is zero per-request overhead and routes outside the scope are unaffected. Global `app.use` middleware always wraps scoped middleware (runs outermost).

## Free functions

| Function | Signature | Description |
| --- | --- | --- |
| `offload` | `offload_awaitable offload(std::function<void()> fn)` | `co_await` to move CPU-heavy work to a stealable compute task, then resume. Header: `include/vthread_scheduler.hpp`. |

```cpp
app.get("/report", [](Request&, Response& res) -> vthread {
    std::string out;
    co_await offload([&]{ out = build_report(); }); // runs off the I/O path
    res.text(out);
});
```

## Validation verbs

Opt in by specializing `swiftnet::schema<T>`. Header: `include/schema.hpp`, `include/detail/validate.hpp`.

```cpp
struct Signup {
    std::string name;
    int age{};
    std::string email;
    std::string role;
    std::optional<std::string> nickname;
};

template <>
struct swiftnet::schema<Signup> {
    static constexpr auto rules = swiftnet::rules(
        field<&Signup::name>(min_len(1), max_len(50)),
        field<&Signup::age>(range(13, 150)),
        field<&Signup::email>(pattern(R"(^[^@]+@[^@]+$)")),
        field<&Signup::role>(one_of("user", "admin")),
        field<&Signup::nickname>(required{})
    );
};
```

| Verb | Signature | Description |
| --- | --- | --- |
| `schema<T>` | `template <class T> struct schema;` | Specialize with `static constexpr auto rules = ...`. |
| `rules` | `rules(Fs... fields)` | Container for one or more `field(...)` entries. |
| `field<&T::m>` | `field<MemPtr>(Cs... constraints)` | Bind constraints to a member (field name derived at compile time). |
| `required{}` | constraint literal | Fail if a `std::optional<T>` member is absent. No-op on non-optional members. |
| `min(v)` / `max(v)` | constraint | Numeric lower / upper bound. |
| `range(lo, hi)` | constraint | Numeric inclusive range. |
| `min_len(n)` / `max_len(n)` | constraint | String length lower / upper bound. |
| `len(lo, hi)` | constraint | String length inclusive range. |
| `pattern("regex")` | constraint | String must match the regex. |
| `one_of(...)` | constraint | String or numeric value must be in the allowed set. |

> ⚠️ A constraint applied to the wrong field type is a **compile error** (e.g. `min()` on a string). `required{}` is only meaningful on `std::optional<T>`; on any other member it silently does nothing.

### Result and error shape

`validate<T>()` returns `Validated<T>{ bool ok; T value; std::vector<FieldError> errors; }`, where `FieldError{ std::string field; std::string rule; std::string message; }`. A type with no `schema<T>` specialization validates as `ok = true`.

`bind<T>(res)` collects all violations and writes this body on failure (HTTP 400):

```json
{
  "error": "validation_failed",
  "details": [
    { "field": "age", "rule": "range", "message": "age must be in [13, 150]" }
  ]
}
```

Error `rule` names: `required`, `min`, `max`, `range`, `min_length`, `max_length`, `length`, `pattern`, `one_of`.

## Common pitfalls

- `body<T>()` never throws. On malformed input it returns a default-constructed `T`, so check fields (or use `validate<T>()` / `bind<T>()`) rather than relying on exceptions.
- `required{}` does nothing on non-optional members — they are always "present". Use it only on `std::optional<T>` fields.
- `Scope::get<T>` returns `nullptr` on a missing key *or* a type mismatch; both are safe misses, not errors (debug builds log them). Always check for `nullptr`.
- `use_async` middleware runs **before** routing; sync `use` middleware runs as part of the chain. Global `use` wraps scoped middleware.
- Skipping `next()` in a `middleware_t` short-circuits the request — make sure you write a response first.

## See also

- [Routing](../guides/routing.md)
- [Requests and responses](../guides/requests-and-responses.md)
- [Validation](../guides/validation.md)
- [Plugins](../guides/plugins.md)
- [Async and offload](../guides/async-and-offload.md)
