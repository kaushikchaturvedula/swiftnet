# Requests & responses

Every SwiftNet handler is called with two objects: a `Request` you read the incoming HTTP message from, and a `Response` you build the reply on. This page is a reference for both APIs.

A handler is either a synchronous `void(Request&, Response&)` or an async `vthread(Request&, Response&)` coroutine. Both receive the same `Request` and `Response`.

## Quick start

```cpp
#include <swiftnet.hpp>

using namespace swiftnet;

int main() {
    SwiftNet app;

    app.get("/users/:id", [](Request& req, Response& res) {
        std::string id   = req.param("id");      // route parameter
        std::string sort = req.query("sort");    // ?sort=...

        res.status(200)
           .header("X-Source", "swiftnet")
           .json(Json{{"id", id}, {"sort", sort}});
    });

    app.post("/echo", [](Request& req, Response& res) {
        res.ok(req.body());                      // echo the raw body back
    });

    app.listen(8080, [] {
        // server is up
    });
}
```

## Request

`Request` exposes the parsed request line, headers, query string, route parameters, and the body in three forms (raw, dynamic JSON, typed).

```cpp
app.post("/users", [](Request& req, Response& res) {
    req.method();              // "POST"
    req.path();                // "/users"
    req.header("Content-Type"); // header lookup, case-insensitive name
    req.query("page");         // "" if absent
    req.param("id");           // route param for a :id segment, "" if absent

    const std::string& raw = req.body();  // raw body string

    Json doc = req.json();     // dynamic nlohmann::json
    User u   = req.body<User>(); // typed parse via Glaze
});
```

### Reading the body

There are three ways to read the body. Pick the one that matches how much structure you need.

| Call | Returns | On malformed input |
| --- | --- | --- |
| `body()` | `const std::string&` (raw bytes) | n/a — returns whatever was sent |
| `json()` | `Json` (`nlohmann::json`) | returns a null/empty `Json` |
| `body<T>()` | `T` parsed via Glaze | returns a **default-constructed `T`** (never throws) |

> ⚠️ `body<T>()` never reports parse errors. On malformed or incompatible input it returns a default-constructed `T`, so a missing field looks identical to a zero/empty value. When the distinction matters, use `validate<T>()` or `bind<T>(res)` (see [validation](validation.md)) instead of inspecting the parsed value yourself.

### Request reference

| Method | Signature | Notes |
| --- | --- | --- |
| `method()` | `const std::string&` | HTTP verb, e.g. `"GET"` |
| `path()` | `const std::string&` | request path, no query string |
| `header(name)` | `std::string` | single header value, `""` if absent |
| `headers()` | `const std::unordered_map<std::string, std::string>&` | full map, built lazily on first call |
| `query(name)` | `std::string` | query-string param, `""` if absent |
| `param(name)` | `std::string` | route param for a `:name` segment, `""` if absent |
| `body()` | `const std::string&` | raw body |
| `is_json()` | `bool` | true when the content type is JSON |
| `json()` | `Json` | dynamic parse (nlohmann) |
| `body<T>()` | `T` | typed parse via Glaze, no throw |
| `validate<T>()` | `Validated<T>` | parse + run `schema<T>` rules |
| `bind<T>(res)` | `std::optional<T>` | parse + validate; writes a 400 and returns `nullopt` on failure |
| `form()` | `std::unordered_map<std::string, std::string>` | parsed form body |
| `has_file(field)` | `bool` | true if a multipart file field is present |

> The header map is only materialized when you call `headers()`. Calling `header(name)` scans on demand and does not build the full map, so the common path (which reads few or no headers) stays cheap.

## Response

`Response` is built fluently: most methods return `Response&`, so you can chain `status`, `header`, and a body setter in one expression.

```cpp
app.get("/health", [](Request&, Response& res) {
    res.status(200)
       .header("Cache-Control", "no-store")
       .json(Json{{"status", "ok"}});
});
```

### Setting the body

There are three body setters, mirroring the request side:

```cpp
res.text("plain text reply");          // text/plain

res.json(Json{{"ok", true}});          // dynamic JSON (nlohmann)

User u{.id = 1, .name = "Ada", .active = true};
res.json(u);                           // typed JSON via Glaze
```

The typed `json(const T&)` overload is selected only when `T` is a `GlazeSerializable` struct, so it never collides with `json(const Json&)` or `text()`. See [typed JSON](typed-json.md) for how the struct overloads are reflected.

### Status helpers

These set a status code and (where shown) a body in one call. Each returns `Response&` so it can end a chain.

| Helper | Status | Argument |
| --- | --- | --- |
| `ok(content = "")` | 200 | body string |
| `created(data = Json{})` | 201 | JSON body |
| `bad_request(message = "Bad Request")` | 400 | message string |
| `unauthorized(message = "Unauthorized")` | 401 | message string |
| `forbidden(message = "Forbidden")` | 403 | message string |
| `not_found(message = "Not Found")` | 404 | message string |
| `internal_error(message = "Internal Server Error")` | 500 | message string |

```cpp
app.get("/users/:id", [](Request& req, Response& res) {
    if (req.param("id").empty()) {
        res.bad_request("id is required");
        return;
    }
    res.not_found("no such user");
});
```

### Response reference

| Method | Signature | Notes |
| --- | --- | --- |
| `status(code)` | `Response&` | set status, chainable |
| `status()` | `int` | read current status |
| `header(k, v)` | `Response&` | set one header |
| `headers(map)` | `Response&` | set many headers |
| `text(content)` | `Response&` | `text/plain` body |
| `html(content)` | `Response&` | `text/html` body |
| `json(const Json&)` | `Response&` | dynamic JSON body |
| `json(const T&)` | `Response&` | typed JSON body (`GlazeSerializable T`) |
| `send(content)` | `Response&` | write a body string |
| `file(path)` | `Response&` | serve a file from disk |
| `redirect(url, code = 302)` | `Response&` | redirect response |
| `cookie(name, value, path = "/", max_age = 0)` | `Response&` | set a cookie |
| `end()` | `Response&` | finish now (used to short-circuit from middleware) |
| `ended()` | `bool` | true once the response is ended |

> `end()` is how async middleware short-circuits a request: ending the response stops the chain before the route handler runs. See [middleware](middleware.md).

## Common pitfalls

- **Treating a default-constructed `T` as a valid parse.** `body<T>()` returns a zero-valued `T` on bad input. Use [validation](validation.md) when you need to reject malformed bodies.
- **Calling `header(name)` in a tight loop expecting a map.** Each call scans the source headers. If you need several headers, call `headers()` once and read from the returned map.
- **Forgetting to return after a status helper.** Helpers like `not_found()` set the response but do not stop your handler. In a sync handler, `return` after them so later code does not overwrite the body.
- **Mixing body setters.** Calling `text()` and then `json()` replaces the body and content type; the last setter wins. Set the body once.
- **Assuming `query()`/`param()` throw on a miss.** Both return `""` for an absent key, so check for emptiness rather than catching exceptions.

## See also

- [Routing](routing.md) — defining routes and `:param` segments that `param()` reads.
- [Typed JSON](typed-json.md) — the Glaze struct overloads behind `body<T>()` and `json(struct)`.
- [Validation](validation.md) — `validate<T>()`, `bind<T>(res)`, and the 400 error body shape.
- [Middleware](middleware.md) — using `Response::end()` to short-circuit a request.
