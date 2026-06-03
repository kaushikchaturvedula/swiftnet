# SwiftNet

A high-performance C++20/23 coroutine web framework: an Express/Fastify-style API on top of a
**per-core, shared-nothing, lock-free** runtime. Each CPU core runs one engine that owns its own
connections, run queue, and I/O reactor — no global queues, no shared mutable state on the request
hot path. The best I/O backend, SIMD path, and core-pinning for the machine are **auto-detected and
embedded** (not knobs); everything that depends on your workload or deployment is a documented knob.

> **Honesty note.** Performance is measured only on the hardware the authors can run: **Apple Silicon
> (M1 Pro, arm64) with the kqueue backend.** The Linux (io_uring/epoll) and Windows (IOCP) backends are
> implemented and functionally tested but **UNVERIFIED for throughput** — no speed is claimed for them.
> [BENCHMARKS.md](BENCHMARKS.md) is the single source of truth for numbers and methodology.

---

## Install

Requires a C++23 compiler (Clang ≥ 17 / GCC ≥ 13; tested on Apple Clang 21) and CMake ≥ 3.20.
Dependencies are fetched/vendored automatically: nlohmann/json + Glaze (JSON), spdlog (logging),
rapidyaml (config, vendored), doctest (tests), and liburing on Linux if present.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On Apple Silicon, also pass `-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"`.

To use it from your own CMake project, add this repo via `add_subdirectory()` (or FetchContent) and
link `swiftnet`.

---

## Hello, world

Full source: [examples/hello.cpp](examples/hello.cpp) (built in CI as the `hello` target).

```cpp
#include "swiftnet.hpp"
using namespace swiftnet;

int main() {
    SwiftNet app(8080);
    app.get("/", [](Request&, Response& res) { res.text("Hello, World!"); });
    app.get("/json", [](Request&, Response& res) {
        Json j; j["message"] = "Hello, World!";
        res.json(j);                 // dynamic JSON (nlohmann)
    });
    app.listen([]{ /* listening on :8080 */ });
}
```

## Typed JSON (Fastify/FastAPI/Spring-style)

Define a plain struct — **Glaze** reflects it at compile time (no schema, no macros) and
(de)serializes at native speed on the hot path. Full source:
[examples/typed_json.cpp](examples/typed_json.cpp) (`typed_json` target).

```cpp
struct User { int id{}; std::string name; bool active{}; };

app.get("/users/sample", [](Request&, Response& res) {
    res.json(User{1, "ada", true});          // typed serialize (Glaze)
});

app.post("/users", [](Request& req, Response& res) {
    User u = req.body<User>();               // typed parse (Glaze); default-constructed on bad input
    if (u.name.empty()) { res.bad_request("name is required"); return; }
    res.status(201).json(u);
});
```

`Json` (nlohmann) is still there for dynamic documents — `res.json(Json{...})` and `req.json()`. The
typed `json<T>` overload is constrained so it never competes with the dynamic one or with `text()`.

## Async handlers and CPU offload

Handlers may be coroutines that `co_await` async I/O. For CPU-heavy work, `co_await
swiftnet::offload(fn)` moves it to a **stealable compute task** so it doesn't block the engine's
I/O-bound connections (see the [work-stealing valve](#work-stealing-valve)):

```cpp
app.get("/work", [](Request&, Response& res) -> vthread {
    std::uint64_t acc = 0;
    co_await swiftnet::offload([&]{ /* heavy compute */ });
    Json j; j["acc"] = acc; res.json(j);
    co_return;
});
```

---

## Architecture: per-core, shared-nothing, lock-free

This is **how SwiftNet works**, not a mode you select — there is no shared global-queue scheduler and
no toggle for one.

- **One engine per core.** Each engine is a pinned thread that fuses the I/O reactor with the worker:
  it owns its own event loop, its own run queue, and the connections it accepts.
- **Kernel connection sharding via `SO_REUSEPORT`** — every engine has its own listener, so accepts
  spread across cores with no shared accept lock.
- **Connections are pinned.** A connection's coroutine, its fds, and its pending I/O all live on the
  engine that accepted it and always resume there, so the per-request path takes **no locks**.
- **Cross-thread work** (e.g. `schedule()` from another thread) is injected through a lock-free MPSC
  inbox and drained by the owning engine.
- **I/O coroutines are never stolen.** Only explicit compute tasks (`offload`) are stealable — moving a
  pinned I/O coroutine would arm the wrong engine's reactor.

Routing uses a compiled **radix tree** (static / `:param` / `*`), not per-request regex.

---

## Auto-detection (embedded, not overridable)

At startup SwiftNet detects the OS, kernel, CPU arch, core topology, and available kernel/CPU features,
then **embeds the universally-best choice** for that machine and logs it. These are not knobs —
exposing them would only invite misconfiguration. Detection **fails safe** to the most portable correct
option.

| Machine class | I/O backend | SIMD | Core pinning |
|---|---|---|---|
| Linux, io_uring probe OK (kernel + liburing + not sandboxed) | **io_uring** | NEON (arm64) / AVX2·SSE2 (x86) | yes (if permitted) |
| Linux, io_uring probe fails (old kernel / seccomp / container) | **epoll** | NEON / AVX2·SSE2 | yes (if permitted) |
| macOS (Apple Silicon / Intel) | **kqueue** | NEON / AVX2·SSE2 | **no** — `KERN_NOT_SUPPORTED` (never faked) |
| Windows | **IOCP** | AVX2·SSE2 | yes |
| anything inconclusive | **epoll / scalar** | scalar | no |

The io_uring choice comes from an **actual `io_uring_queue_init` probe**, not a version guess, so a
container whose seccomp policy blocks io_uring transparently falls back to epoll. The selected backend
is logged at startup with a `VERIFIED`/`UNVERIFIED` tag, e.g.:

```
SwiftNet runtime: os=macOS (25.5.0) arch=arm64 cores=10 logical/10 physical
  topology: P-cores=8 E-cores=2
  backend: kqueue [VERIFIED]   simd: NEON
  pinning: DISABLED (macOS: KERN_NOT_SUPPORTED)
SwiftNet config: engines=10 port=8080 backlog=1024
  valve: OFF (threshold=1 max_batch=1 min_idle=0)   limits: max_header=65536B max_body=8388608B  log_level=info
```

---

## Configuration

Precedence: **built-in defaults → programmatic (code) → YAML file → environment variables.**
**Environment always wins.** YAML is optional (path from `SWIFTNET_CONFIG`, else `./swiftnet.yaml`);
a missing file is skipped and a malformed one is logged and ignored (never crashes).

| Knob | Env var | YAML key | Default | Range | Platform |
|---|---|---|---|---|---|
| Engine count | `SWIFTNET_ENGINES` | `engines` | all logical cores | 1..logical | all |
| Listen port | `SWIFTNET_PORT` | `port` | 8080 | 1..65535 | all |
| Accept backlog | `SWIFTNET_BACKLOG` | `backlog` | 1024 | ≥1 | all |
| Work-steal valve | `SWIFTNET_STEAL` | `steal` | off | 0/1 | all |
| Steal threshold (victim depth) | `SWIFTNET_STEAL_THRESHOLD` | `steal_threshold` | 1 | ≥0 | all |
| Steal max batch / turn | `SWIFTNET_STEAL_MAX_BATCH` | `steal_max_batch` | 1 | ≥1 | all |
| Min idle engines before stealing | `SWIFTNET_STEAL_MIN_IDLE` | `steal_min_idle` | 0 | 0..engines | all |
| Max request header bytes | `SWIFTNET_MAX_HEADER_BYTES` | `max_header_bytes` | 65536 | 1KiB..1MiB | all |
| Max request body bytes | `SWIFTNET_MAX_BODY_BYTES` | `max_body_bytes` | 8 MiB | 0..2GiB | all |
| Log level | `SWIFTNET_LOG_LEVEL` | `log_level` | info | trace/debug/info/warn/error | all |
| io_uring provided buffers | `SWIFTNET_IOURING_PROVIDED_BUFFERS` | `iouring_provided_buffers` | off | 0/1 | Linux (reserved, UNVERIFIED) |
| I/O backend · SIMD · pinning | — | — | **auto-detected** | — | embedded (logged, not overridable) |

Example `swiftnet.yaml`:

```yaml
engines: 8
port: 8080
backlog: 1024
steal: false
steal_threshold: 2
max_body_bytes: 1048576
log_level: info
```

Programmatic seeds (overridden by YAML/env): `SwiftNet app(port); app.set_threads(n); app.set_backlog(b);`

---

## Work-stealing valve

Off by default. The per-core model wins on cache locality under balanced load, but under **imbalance**
(one engine buried in CPU-heavy `offload` work while others sit idle) that advantage erodes — so the
valve lets an idle engine **steal a compute task** off a busy one. It is **compute-only**; pinned I/O
is never moved. Knobs: `steal` (on/off), `steal_threshold` (victim depth before stealing),
`steal_max_batch` (tasks run per engine turn), `steal_min_idle` (idle engines required before
stealing). Honest framing: the valve **redistributes spare capacity, it does not create it** — its
benefit shrinks as all cores get busy. Measured OFF/ON/INLINE deltas are in
[BENCHMARKS.md §4](BENCHMARKS.md) (it relieves light-connection tail latency dramatically *given*
offload; it cannot help work that runs inline in a pinned coroutine).

---

## Verified vs unverified (per platform/backend)

| Platform / backend | Status | What's verified |
|---|---|---|
| **macOS / kqueue** (Apple Silicon) | ✅ **Measured** | All BENCHMARKS.md numbers; ctest; ThreadSanitizer-clean under load |
| **Linux / io_uring** | ⚙️ Functional, **throughput UNVERIFIED** | Compiles + full test suite + live requests in a Linux container; per-engine timers + eventfd wake + `COOP_TASKRUN`. Multishot accept / provided buffers are documented future work |
| **Linux / epoll** | ⚙️ Functional, **throughput UNVERIFIED** | Auto-selected when io_uring is unavailable; compiles + test suite + live requests in a container |
| **Windows / IOCP** | 🚧 **Skeleton, UNVERIFIED** | Compiles as a shape behind the backend interface; a real `OVERLAPPED` + `WSARecv/WSASend` completion path (and the Windows `tcp_socket` integration) is **not yet done** — the authors have no Windows toolchain to compile or run it |

No throughput/latency number is reported for any backend except kqueue. See
[BENCHMARKS.md](BENCHMARKS.md).

---

## Building & testing

```bash
# Release (default)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure          # unit, detect, config, simd, glaze, integration

# Sanitizers (gate the concurrency-critical paths)
cmake -S . -B build-tsan -DSWIFTNET_SANITIZE=thread  && cmake --build build-tsan -j
cmake -S . -B build-asan -DSWIFTNET_SANITIZE=address && cmake --build build-asan -j
```

CMake options: `SWIFTNET_NATIVE` (host-CPU tuning, default ON — auto-skipped if the toolchain rejects
it), `SWIFTNET_LTO` (default OFF), `SWIFTNET_SANITIZE` (`none|address|thread|undefined`),
`SWIFTNET_BUILD_EXAMPLES`, `SWIFTNET_BUILD_TESTS`.

Benchmark harness and the off-host load-generator kit live in [benchmark/](benchmark/); the Linux
io_uring/epoll functional check runs in Docker (see [BENCHMARKS.md](BENCHMARKS.md) → Reproduce).
