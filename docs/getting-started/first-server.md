# Your first server

A minimal SwiftNet program: a couple of routes, one line to start listening, and `curl` to see it respond. By the end you'll understand the three steps every SwiftNet app takes - create the app, register routes, call `listen()`.

> This page assumes you've already built SwiftNet. If not, start with [installation.md](installation.md).

## Quick start

Create `hello.cpp`:

```cpp
#include "swiftnet.hpp"

using namespace swiftnet;

int main()
{
    SwiftNet app(8080);

    // Plain text response.
    app.get("/", [](Request &, Response &res) {
        res.text("Hello, World!");
    });

    // Dynamic JSON response.
    app.get("/json", [](Request &, Response &res) {
        Json j;
        j["message"] = "Hello, World!";
        res.json(j);
    });

    // Blocks until SIGINT (Ctrl-C) or close().
    // The runtime/config banner is logged at startup.
    app.listen([] { /* listening on :8080 */ });
    return 0;
}
```

That's the whole program. `SwiftNet app(8080)` constructs the app bound to port `8080`, each `get(...)` registers a route, and `listen()` starts the server and blocks the main thread until you stop it.

> This exact file ships in the repo as `examples/hello.cpp` and is built in CI, so it can't go stale.

## Building and running

The example is wired up as a CMake target named `hello`. From a configured build directory:

```bash
cmake --build . --target hello
./examples/hello
```

When the server starts it logs a one-time banner describing the chosen runtime - the I/O backend (`kqueue` on macOS), the SIMD level, core pinning, and the active config - each tagged `VERIFIED` or `UNVERIFIED`. On macOS that backend line reads `kqueue` and is tagged `VERIFIED`.

Stop the server with Ctrl-C.

## Try it with curl

In a second terminal, hit each route:

```bash
curl localhost:8080/
# Hello, World!
```

```bash
curl localhost:8080/json
# {"message":"Hello, World!"}
```

## How it works

- `SwiftNet app(8080)` creates the app. The constructor signature is `SwiftNet(uint16_t port = 8080)`, so `SwiftNet app;` would also bind `8080`.
- `app.get("/", handler)` registers a `GET` route. Each verb method returns `SwiftNet&`, so calls can be chained.
- A handler is a callable taking `(Request&, Response&)`. The two shown here are *sync* handlers (they return `void`). SwiftNet also accepts *async* handlers that return `vthread` and `co_await` I/O - both styles are adapted automatically. See [../guides/requests-and-responses.md](../guides/requests-and-responses.md).
- `res.text(...)` sets a plain-text body; `res.json(const Json&)` serializes a dynamic `Json` value (which is `nlohmann::json`) and sets `Content-Type: application/json`.
- `app.listen(callback)` starts the server. On startup SwiftNet spins up one I/O engine per logical core by default (configurable - see below), then blocks the calling thread until `close()` or a signal. The optional callback runs once the server is listening.

When a request arrives, an engine reads it off the socket, the router matches the method and path to your handler, the handler writes into the `Response`, and the engine writes the bytes back to the client.

## Going async (optional)

The same routes can be written as coroutine handlers when you need to `co_await` async work. Both forms register through the same `get`/`post`/etc. methods:

```cpp
app.get("/async", [](Request &, Response &res) -> vthread {
    // co_await async I/O here ...
    res.text("done");
    co_return;
});
```

## Response basics

A few of the methods you'll reach for first. All are chainable (each returns `Response&`):

| Call | Effect |
| --- | --- |
| `res.text(str)` | Set a plain-text body |
| `res.json(const Json& j)` | Serialize a dynamic `Json` body, set JSON content type |
| `res.json(const T& v)` | Serialize a typed struct (see [../guides/typed-json.md](../guides/typed-json.md)) |
| `res.status(int code)` | Set the HTTP status code |
| `res.header(k, v)` | Set a response header |
| `res.not_found(msg)` | Convenience for a 404 response |

## Common pitfalls

- **Forgetting `using namespace swiftnet;`.** Without it, write `swiftnet::SwiftNet`, `swiftnet::Request`, `swiftnet::Json`, and so on. The snippets on this site assume the `using` directive is present.
- **Expecting `listen()` to return.** `listen()` blocks until the server shuts down. Put any post-startup logic in the `listen` callback, not after the call.
- **Address already in use.** If the port is taken, change it - `SwiftNet app(3000)`, or set `SWIFTNET_PORT`. Environment variables override the port passed in code (see [installation.md](installation.md) for the config precedence).
- **Confusing the two `json` overloads.** `res.json(j)` for a dynamic `Json`; `res.json(myStruct)` for a typed aggregate. Picking the typed overload requires a `GlazeSerializable` struct - covered in [../guides/typed-json.md](../guides/typed-json.md).

## See also

- [installation.md](installation.md) - build SwiftNet and its dependencies.
- [../guides/routing.md](../guides/routing.md) - path params, route verbs, and matching.
- [../guides/requests-and-responses.md](../guides/requests-and-responses.md) - reading requests and shaping responses, sync and async.
- [../guides/typed-json.md](../guides/typed-json.md) - parse and serialize structs with zero macros.
