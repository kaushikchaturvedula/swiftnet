# Platform & backend support

SwiftNet runs on one cross-platform reactor that picks an I/O backend per machine at startup. This page is the authoritative status matrix for each backend: what is verified, what is functional but unmeasured, and what is still a skeleton.

> SwiftNet has only ever been benchmarked on one backend. No throughput or latency number is reported for any backend except macOS/kqueue. See [Benchmarks](../benchmarks.md).

## Quick start

You do not choose a backend. The reactor is selected automatically at startup and logged in the runtime banner, so the simplest server already runs on the right backend for the host.

```cpp
#include <swiftnet.hpp>
using namespace swiftnet;

int main() {
    SwiftNet app;

    app.get("/", [](Request& req, Response& res) {
        res.text("Hello from SwiftNet");
    });

    // On startup the banner logs the chosen I/O backend (kqueue / io_uring /
    // epoll / IOCP), the SIMD path, core-pinning, and a VERIFIED/UNVERIFIED tag.
    app.listen(8080, [] {
        // server is listening
    });
}
```

> The backend, SIMD path, and core-pinning are auto-detected, embedded, and logged. They are not overridable by config or environment variable. For how the choice is made, see [Auto-detection](../architecture/auto-detection.md).

## How it works

- One `event_loop` façade owns exactly one platform backend, chosen when the loop is constructed.
- The backend is fixed at compile time per platform (kqueue on macOS, IOCP on Windows) and chosen at runtime on Linux (io_uring when the kernel probe succeeds, otherwise epoll).
- The reactor is one-shot: a watch fires once and is removed by the kernel; to wait on the same fd again you re-arm. This matches the "co_await arms once, resumes once" model of the virtual-thread runtime.
- Every backend implements the same interface, so handlers, typed JSON, validation, and the scheduler behave identically regardless of which backend is active.
- The startup banner tags the backend `VERIFIED` only when it is kqueue; every other backend is tagged `UNVERIFIED`.

## Backend status matrix

| Platform | Backend | Status | What that means |
|---|---|---|---|
| macOS / BSD (Apple Silicon) | kqueue | **VERIFIED** | The primary, fully exercised target. All numbers in [Benchmarks](../benchmarks.md) were measured here (Apple M1 Pro, arm64). `ctest` passes and the runtime is ThreadSanitizer-clean under load. |
| Linux | io_uring | **Functional; throughput UNVERIFIED** | Compiles, passes the full test suite, and serves live requests in a Linux container. Per-engine timers, `eventfd` wake, and `IORING_SETUP_COOP_TASKRUN` are enabled; `SINGLE_ISSUER`/`DEFER_TASKRUN` are deliberately not used. Multishot accept and provided buffers are future work. No speed is claimed. |
| Linux | epoll | **Functional; throughput UNVERIFIED** | Auto-selected when io_uring is unavailable (old kernel, seccomp, or container where the `io_uring_queue_init` probe fails). Compiles, passes the full test suite, and serves live requests. No speed is claimed. |
| Windows | IOCP | **Skeleton; UNVERIFIED** | A skeleton behind the backend interface. There is no real `OVERLAPPED` + `WSARecv`/`WSASend` completion path yet, and the authors have no Windows toolchain to validate it. Do not rely on it. |

> ⚠️ Do not infer relative performance from this table. "Functional" means the backend compiles, passes tests, and serves correct responses — not that it is fast or slow. The only backend with measured numbers is kqueue, and even those are per-request latency and CPU-per-request, not a web-throughput claim. See [Benchmarks](../benchmarks.md).

## How a backend is selected

- **macOS:** kqueue, always (the only VERIFIED backend).
- **Linux:** io_uring when an actual `io_uring_queue_init` probe succeeds and `liburing` was found at build time; otherwise epoll.
- **Windows:** IOCP (skeleton).
- **Anything inconclusive:** epoll, the most portable correct fallback.

Core-pinning is enabled on Linux and Windows when an affinity probe succeeds, and is always disabled on macOS (`KERN_NOT_SUPPORTED`). The full decision procedure, including SIMD selection, lives in [Auto-detection](../architecture/auto-detection.md).

## Common pitfalls

- **Trying to force a backend.** The backend is auto-detected and not overridable. There is no `SWIFTNET_BACKEND` knob. If you want to confirm what was chosen, read the startup banner.
- **Reading the Linux io_uring `UNVERIFIED` tag as "broken."** It is functionally tested (compiles, full `ctest`, live requests). The tag means no throughput has been measured, not that it fails.
- **Deploying on Windows.** The IOCP backend is a skeleton with no real completion path. Treat Windows as unsupported for now.
- **Quoting kqueue numbers for other backends.** The measured latency and CPU-per-request figures apply only to macOS/kqueue on Apple Silicon. Do not generalize them.
- **Expecting io_uring `SINGLE_ISSUER`/`DEFER_TASKRUN`.** These are intentionally off; only `COOP_TASKRUN` is enabled. Multishot accept and provided buffers are not yet wired in.

## See also

- [Auto-detection](../architecture/auto-detection.md) — how the backend, SIMD path, and pinning are chosen and logged.
- [Benchmarks](../benchmarks.md) — the single source of truth for measured numbers (kqueue only).
- [Configuration](../guides/configuration.md) — the knobs you can set (engines, port, backlog, limits); the backend is not among them.
