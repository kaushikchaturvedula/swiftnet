# FAQ & troubleshooting

Short answers to the questions that come up most when getting SwiftNet to build, validate, and run. Each answer links to the page with the full story.

## Build & install

### I get a compiler error about coroutines, concepts, or `std::optional` reflection

SwiftNet requires a **C++23 compiler** — Glaze (used for typed JSON) needs it. Use Clang ≥ 17, GCC ≥ 13, or Apple Clang 21, and CMake ≥ 3.20.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

See [getting-started/installation.md](getting-started/installation.md) for the full prerequisite list.

### The build fails on Apple Silicon with a sysroot or architecture error

Pass the macOS SDK path and target arch explicitly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"
cmake --build build -j
```

> A fresh CMake on macOS sometimes can't locate the SDK on its own; `-DCMAKE_OSX_SYSROOT` makes it explicit. macOS/kqueue is the one **VERIFIED** backend, so this is the recommended path for first builds.

### Do I need liburing on Linux?

No. liburing is **optional**. If it's found at build time, SwiftNet compiles in an `io_uring_queue_init` probe and uses io_uring when the probe succeeds at runtime; otherwise it falls back to **epoll** transparently. Without liburing, io_uring simply reports unavailable and epoll is selected. Both Linux backends are functionally tested but **throughput UNVERIFIED**. See [architecture/auto-detection.md](architecture/auto-detection.md).

## Validation

### My `required{}` constraint is not firing

`required{}` only works on `std::optional<T>` fields. On a non-optional member it is a **silent no-op**.

A parsed struct always has every non-optional member (defaulted if the JSON key was absent), so "missing" and "default value" are indistinguishable without re-parsing the raw JSON — which SwiftNet deliberately does not do. Presence is detected by whether the optional is engaged after parsing.

To make a field mandatory, declare it `std::optional<T>` and add `required{}`:

```cpp
struct Signup { std::string name; int age{}; std::string email; std::string role;
                std::optional<std::string> nickname; };

template <> struct swiftnet::schema<Signup> {
    static constexpr auto rules = swiftnet::rules(
        swiftnet::field<&Signup::nickname>(required{}, max_len(20)));  // optional -> required{} fires
};
```

See [guides/validation.md](guides/validation.md).

### My validation rule causes a compile error

That is intentional: applying a constraint to the wrong type is a **compile error**, not a silent failure. For example, `len(...)` on a numeric field or `range(...)` on a string will not compile. Match the rule to the member type — string rules (`min_len`/`max_len`/`len`/`pattern`) on strings, numeric rules (`min`/`max`/`range`) on numbers, `one_of(...)` on either. See [guides/validation.md](guides/validation.md).

## Plugins & decorators

### My decorator `get<T>` returns `nullptr`

`get<T>("key")` returns `std::shared_ptr<const T>`, and it returns `nullptr` for a **safe miss**. The two common causes:

- **Wrong type requested.** A decorator stored as one type and looked up as a different type is a miss. If you stored `decorate<std::shared_ptr<Db>>("db", ...)`, you must read it back as exactly `get<std::shared_ptr<Db>>("db")`.
- **A sibling scope cannot see another scope's decorator.** Lookup resolves **own → parent** (a child inherits parent decorators and may override locally). Siblings are isolated — they never share a store, and lookup never walks sideways.

Debug builds log the miss (key not found vs. type mismatch) to help you trace it. Read decorators **at registration** and capture them into your handler closures — that keeps lookups off the hot path, and the returned `shared_ptr<const T>` is read-only by design.

```cpp
app.plugin([](swiftnet::Scope& v1) {
    v1.decorate<std::shared_ptr<Db>>("db", openDb());
    auto db = v1.get<std::shared_ptr<Db>>("db");        // read once, here
    v1.get("/users", [db](Request&, Response& res) { res.text((*db)->query()); });
}, {.prefix = "/v1"});
```

See [guides/plugins.md](guides/plugins.md).

## Runtime & platform

### Core pinning is disabled on macOS — is that a bug?

No, that is expected. macOS does not expose a thread-affinity API that works for this (the kernel returns `KERN_NOT_SUPPORTED`), so pinning is **always disabled on macOS** and never faked. On Linux and Windows it is enabled when an affinity probe succeeds. The startup banner states the choice explicitly:

```
pinning: DISABLED (macOS: KERN_NOT_SUPPORTED)
```

See [architecture/auto-detection.md](architecture/auto-detection.md).

### Which I/O backend am I running?

Check the **startup banner**, logged once when the server starts. It names the backend and tags it `VERIFIED` or `UNVERIFIED`:

```
SwiftNet runtime: os=macOS (25.5.0) arch=arm64 cores=10 logical/10 physical
  topology: P-cores=8 E-cores=2
  backend: kqueue [VERIFIED]   simd: NEON
  pinning: DISABLED (macOS: KERN_NOT_SUPPORTED)
```

The backend is auto-detected and embedded, not a knob: macOS → kqueue, Linux → io_uring (if the probe succeeds) else epoll, Windows → IOCP, anything inconclusive → epoll. See [architecture/auto-detection.md](architecture/auto-detection.md).

## Performance & status

### Why are there no requests/second numbers?

Because the only available test setup is a **single host**, where the load generator (`wrk`) runs on the same cores as the server over loopback. In that setup the throughput ceiling is set by the **harness and the loopback TCP path, not by SwiftNet** — at the ceiling the server sits ~92% idle, and 2 engines serve as much as 8. So a single-host RPS number measures the harness, not the server.

The honest, host-independent server metrics are **per-request latency** and **CPU-per-request**, both reported on the benchmarks page. A real cross-framework throughput comparison needs a **dedicated off-host load box** (a setup-only kit ships in `benchmark/remote/`); no such numbers exist yet. See [benchmarks.md](benchmarks.md).

> The work-stealing valve experiment in the benchmarks is a **compute-bound scheduler result**, not a web-throughput result. It shows the valve redistributes spare capacity given `offload`; it does not create capacity.

### Is this production ready? Which platforms are verified?

Status is per platform/backend:

| Platform / backend | Status |
|---|---|
| macOS / kqueue (Apple Silicon) | **VERIFIED** — measured on M1 Pro; ctest + ThreadSanitizer-clean under load |
| Linux / io_uring | Implemented, functionally tested (compile + full ctest + live requests in a Linux container); **throughput UNVERIFIED** |
| Linux / epoll | Implemented, functionally tested (same as above); **throughput UNVERIFIED** |
| Windows / IOCP | **Skeleton, UNVERIFIED** — no real `OVERLAPPED` + `WSARecv`/`WSASend` completion path yet; the authors have no Windows toolchain |

Treat macOS/kqueue as the trusted path today; Linux is functional but unmeasured for throughput; Windows is a shape behind the backend interface, not a working server. See [benchmarks.md](benchmarks.md) for the methodology behind these claims.

## Common pitfalls

- **Expecting `required{}` to validate a non-optional field.** It is a no-op there — make the field `std::optional<T>`.
- **Reading a decorator with the wrong type or from a sibling scope.** Both return `nullptr`; check the debug log and verify the exact `T` and scope.
- **Calling `plugin()` after `listen()`.** Decorators and scoped middleware are resolved at registration, so register all plugins before `listen()`.
- **Treating a single-host RPS number as a throughput claim.** It measures the loopback harness; use latency and CPU-per-request instead.
- **Editing the I/O backend, SIMD path, or core pinning via config.** These are auto-detected and embedded, not knobs — confirm the actual choice in the startup banner.

## See also

- [getting-started/installation.md](getting-started/installation.md) — prerequisites and the first build.
- [guides/validation.md](guides/validation.md) — schema constraints, `required{}`, and the 400 error shape.
- [guides/plugins.md](guides/plugins.md) — scopes, decorators, and encapsulation.
- [architecture/auto-detection.md](architecture/auto-detection.md) — backend, SIMD, and pinning selection.
- [benchmarks.md](benchmarks.md) — what is measured, and why there are no RPS numbers.
