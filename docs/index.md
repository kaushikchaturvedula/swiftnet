# Introduction

SwiftNet is a C++23 web framework with an Express/Fastify-style API, built on a per-core, shared-nothing, lock-free runtime. You write ordinary route handlers; SwiftNet runs them on one engine per CPU core, each owning its own connections, run queue, and I/O reactor — no global queues, no shared mutable state on the request hot path.

If you have written a server in Express, Fastify, or FastAPI, the surface here will feel familiar: an app object, route verbs, middleware, typed request/response bodies, and plugins. The difference is what runs underneath — native C++ coroutines on a runtime that picks the best I/O backend, SIMD path, and core-pinning strategy for your machine automatically.

## Quick start

```cpp
#include "swiftnet.hpp"
using namespace swiftnet;

int main() {
    SwiftNet app(8080);

    app.get("/", [](Request&, Response& res) {
        res.text("Hello, World!");
    });

    app.get("/users/sample", [](Request&, Response& res) {
        res.json(User{1, "ada", true});   // typed JSON via Glaze
    });

    app.listen([] { /* listening on :8080 */ });
    return 0;
}
```

`SwiftNet`'s constructor takes a port (`SwiftNet(uint16_t port = 8080)`). Route verbs — `get`, `post`, `put`, `del`, `patch`, `options`, `head` — each take a path and a handler, and return `SwiftNet&` so you can chain them. A handler is a plain `void(Request&, Response&)` lambda, or a coroutine returning `vthread` when you need to `co_await` async work; both styles are accepted.

> The typed-JSON example above uses `struct User { int id{}; std::string name; bool active{}; };`. See [Typed JSON](guides/typed-json.md) for how Glaze reflects plain structs at compile time.

## Who it is for

- C++ teams who want a high-level, ergonomic HTTP API without giving up native performance or control.
- Developers coming from Node/Python frameworks who want the same mental model (routes, middleware, plugins, typed bodies) in C++.
- Workloads that benefit from a per-core, lock-free runtime: many concurrent connections, predictable tail latency, and CPU-bound work that can be offloaded off the I/O path.

## Design philosophy

- **Per-core engines, shared nothing.** Each core runs one pinned engine that fuses the I/O reactor with the worker. A connection is pinned to the engine that accepted it and always resumes there, so the per-request path takes no locks. This is how SwiftNet works, not a mode you select.
- **Auto-detect what the machine dictates; expose what your workload dictates.** The I/O backend (io_uring / epoll / kqueue / IOCP), SIMD path (NEON / AVX2 / SSE2 / scalar), and core-pinning are detected at startup, embedded, and logged — they are not knobs. Everything that depends on your deployment (engine count, port, backlog, limits, the work-stealing valve) is a documented knob with environment-variable override.
- **Honesty about performance.** Throughput is measured only where the authors can run it: macOS / Apple Silicon (M1 Pro) with the kqueue backend, which is `VERIFIED`. The Linux (io_uring / epoll) and Windows (IOCP) backends are implemented to varying degrees but their throughput is `UNVERIFIED`. There are no cross-framework comparisons or requests/second rankings. See [Benchmarks](benchmarks.md) for measured latency and CPU-per-request data and methodology.

> ⚠️ Only the macOS/kqueue backend is `VERIFIED`. The Linux backends compile, pass the full test suite, and serve live requests in a container, but no throughput is claimed for them. Windows/IOCP is a skeleton with no completion path yet. Check [Architecture overview](architecture/overview.md) for per-backend status before deploying.

## Feature map

| Feature | What you get | Guide |
|---|---|---|
| Routing | Radix-tree matcher with static, `:param`, and `*` segments | [Routing](guides/routing.md) |
| Typed JSON | Define a plain struct; Glaze (de)serializes it at compile time, no macros | [Typed JSON](guides/typed-json.md) |
| Validation | Opt-in `schema<T>` constraints (required, ranges, length, regex, enum); structured 400 errors | [Validation](guides/validation.md) |
| Plugins & scopes | Prefix-grouped routes, scoped middleware, and typed decorators with Fastify-style encapsulation | [Plugins](guides/plugins.md) |
| Middleware | Express-style `use(mw)` (call `next()` to continue), path-scoped `use(prefix, mw)`, and `use_async` | [Routing](guides/routing.md) |
| Async & offload | Coroutine handlers; `co_await swiftnet::offload(fn)` moves CPU-heavy work to a stealable compute task | [Architecture overview](architecture/overview.md) |
| Configuration | Defaults to YAML to environment variables (env always wins) | [Configuration](guides/configuration.md) |

## Next steps

- New here? Start with [Installation](getting-started/installation.md) to get a build.
- Then walk through [Your first server](getting-started/first-server.md) end to end.
- Want the full picture of the runtime? Read the [Architecture overview](architecture/overview.md).

## See also

- [Installation](getting-started/installation.md)
- [Your first server](getting-started/first-server.md)
- [Routing](guides/routing.md)
- [Typed JSON](guides/typed-json.md)
- [Validation](guides/validation.md)
- [Plugins](guides/plugins.md)
- [Configuration](guides/configuration.md)
- [Architecture overview](architecture/overview.md)
- [Benchmarks](benchmarks.md)
- [FAQ](faq.md)
