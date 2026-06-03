# Routing

SwiftNet maps incoming requests to handlers using the HTTP method and the URL path. Routes are registered on your `SwiftNet` app with one method call per verb, and matching is done by a compiled radix tree, so route order does not matter.

## Quick start

```cpp
#include <swiftnet.hpp>

using namespace swiftnet;

int main() {
    SwiftNet app;

    app.get("/health", [](Request& req, Response& res) {
        res.text("ok");
    });

    // ":id" captures a path segment; read it with req.param("id").
    app.get("/users/:id", [](Request& req, Response& res) {
        res.json(Json{{"id", req.param("id")}});
    });

    app.listen(8080, [] {
        // server is listening
    });
}
```

## How it works

- **One method per HTTP verb.** `app.get`, `app.post`, `app.put`, `app.del`, `app.patch`, `app.options`, and `app.head` each take `(const std::string& path, handler)` and return `SwiftNet&`, so calls chain.
- **Handlers are sync or async.** A handler is either a sync `void(Request&, Response&)` or an async coroutine `vthread(Request&, Response&)`. Both are accepted on every verb and adapted automatically.
- **Three segment kinds.** A path is split on `/` into segments. Each segment is either a static literal (`users`), a named parameter (`:id`), or the trailing wildcard (`*`).
- **Read params from the `Request`.** Captured parameters are available via `req.param("name")`, which returns the matched segment as a `std::string` (empty string if the name was not captured).
- **Radix-tree matching, not regex.** Routes compile into a shared radix tree. Matching is an `O(path-depth)` tree walk with no per-request regular-expression evaluation. At each node the router prefers a static child first, then a `:param` child, then a `*` wildcard, backtracking as needed.
- **Order-independent.** Because precedence is static > param > wildcard at every node, registration order does not change which route wins. `/users/me` and `/users/:id` coexist correctly no matter which you register first; `/users/me` always takes the static branch.

> Performance for routing is reported as per-request latency and CPU-per-request, measured only on macOS/kqueue. See [the benchmarks page](../benchmarks.md) for what is and is not measured. There are no requests-per-second claims here.

## Path syntax

| Syntax | Matches | Example pattern | Matches path | `req.param(...)` |
| --- | --- | --- | --- | --- |
| `literal` | exactly that segment | `/users` | `/users` | — |
| `:name` | exactly one segment, captured | `/users/:id` | `/users/42` | `param("id")` → `"42"` |
| `*` | the rest of the path (trailing only) | `/files/*` | `/files/a/b.txt` | — |

A pattern may mix kinds across segments, for example `/teams/:team/users/:id`. Each `:name` captures exactly one segment.

> ⚠️ The `*` wildcard is only meaningful as the final segment. It matches whatever remains of the path but does not expose that remainder as a named param; use `:name` segments for the parts you need to read back.

> Paths are matched against the request path with the query string already stripped, so `GET /q?name=ada` is matched against the pattern `/q`. Read query parameters with `req.query("name")`, not `req.param`.

## Reading route parameters

`req.param(name)` returns the captured value for a `:name` segment:

```cpp
using namespace swiftnet;

app.get("/teams/:team/users/:id", [](Request& req, Response& res) {
    std::string team = req.param("team");
    std::string id   = req.param("id");
    res.json(Json{{"team", team}, {"user", id}});
});
```

> A `:name` segment always matches a non-empty segment. `req.param` returns an empty string only when you ask for a name the matched route never declared.

## 404 behavior

If no route in the tree matches the request's method and path, SwiftNet does not call a handler. It returns a `404` response whose body is `Route not found: <METHOD> <path>`:

```
HTTP/1.1 404 Not Found
Content-Type: text/plain

Route not found: GET /missing
```

Method matters: a path registered only for `GET` will return `404` for a `POST` to the same path. If you want a catch-all, register a route ending in `*` (for example `app.get("/api/*", ...)`); it matches any remaining path under that prefix but still only for the verb you registered it on.

## Common pitfalls

- **Expecting regex-style first-match-wins.** SwiftNet is not regex-ordered. A static segment always beats a `:param` at the same position regardless of registration order, so you cannot "shadow" `/users/:id` by registering it before `/users/me`.
- **Using `*` mid-path.** Only a trailing `*` is supported. Put dynamic middle segments as `:name` instead.
- **Reading a query value with `param`.** Route params come from `:name` segments; query-string values come from `req.query`. They are separate maps.
- **Forgetting the method.** Registering `app.get("/x", ...)` does not make `POST /x` work; add the verb you need (or it 404s).
- **Pathologically deep paths.** The matcher walks up to 32 segments; paths deeper than that do not match and fall through to the 404 path.

## See also

- [Requests and responses](requests-and-responses.md) — reading params, query, headers, and body; sending typed JSON.
- [Plugins](plugins.md) — group routes under a prefix with `app.plugin(fn, { .prefix = "/v1" })`.
- [Request lifecycle](../architecture/request-lifecycle.md) — how a request flows from socket to matched handler.
