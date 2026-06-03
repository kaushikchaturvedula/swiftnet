# Auto-detection

At startup, SwiftNet inspects the machine it is running on and picks the best I/O backend, SIMD path, and core-pinning strategy for that environment. These choices are *embedded* and *logged* — they are deliberately not configuration knobs.

> SwiftNet detects the OS, kernel, architecture, core topology, and CPU/kernel features once per process, then commits to the universally-best choice for that machine. Exposing these as settings would mostly invite misconfiguration, so they are made automatically and printed at startup so the behavior stays transparent.

## How it works

- **Fact-gathering is separate from decision-making.** `gather_facts()` does all the platform-specific work (syscalls, `sysctl`, feature probes); `select()` is a pure function that turns those facts into choices with no I/O. The selection rules are unit-tested against synthetic facts. See `include/detail/runtime_detect.hpp`.
- **The result is computed once and memoized** for the whole process via a thread-safe function-local static (`cached_runtime()`).
- **Detection is fail-safe.** Anything inconclusive falls back to the most portable correct option: `epoll` for the backend, `scalar` for SIMD, and pinning off.
- **The choices feed the event loop, scheduler, and config**, and are emitted as a startup banner.
- **None of this is overridable.** There are no env vars or YAML keys for backend, SIMD, or pinning. (Tunable runtime knobs like engine count, port, and backlog live in [the configuration guide](../guides/configuration.md).)

## Detection table

| Machine class | I/O backend | SIMD | Core-pinning |
| --- | --- | --- | --- |
| Linux, `io_uring` probe succeeds | `io_uring` | per arch (below) | enabled if affinity probe succeeds |
| Linux, probe fails / no `liburing` | `epoll` | per arch (below) | enabled if affinity probe succeeds |
| macOS (Apple Silicon or Intel) | `kqueue` | per arch (below) | **always disabled** (`KERN_NOT_SUPPORTED`) |
| Windows | `IOCP` | per arch (below) | enabled (affinity generally available) |
| Anything inconclusive | `epoll` | `scalar` | disabled |

SIMD is chosen from the architecture and CPU features:

| Architecture | SIMD path |
| --- | --- |
| ARM / `arm64` / `aarch64` | `NEON` (baseline) |
| x86, AVX2 supported | `AVX2` |
| x86, SSE2 only | `SSE2` |
| x86, neither | `scalar` |

## The `io_uring` probe

On Linux, SwiftNet does not assume `io_uring` is usable just because the binary was built with it. The check has two parts:

- `liburing` must have been found **at build time** (the probe code compiles in only under `SWIFTNET_HAS_LIBURING`).
- At startup, `probe_iouring()` performs an **actual `io_uring_queue_init(8, &ring, 0)`** and immediately tears the ring down. Only if that call returns `0` is `io_uring` selected.

This means a sandboxed or seccomp-restricted container, or an older kernel that refuses the syscall, transparently **falls back to `epoll`** — the probe fails, and the fail-safe path takes over. No crash, no manual flag.

## macOS pinning

Core-pinning is **always disabled on macOS**. Apple Silicon returns `KERN_NOT_SUPPORTED` for thread-affinity requests, so `affinity_works` is hard-set to `false` and the selection rule never enables pinning on macOS. The startup banner says so explicitly.

## Startup banner

When the scheduler starts, SwiftNet logs the detected runtime at `info` level. The backend line carries a `[VERIFIED]` or `[UNVERIFIED]` tag. **Only `kqueue` is tagged `VERIFIED`** — every other backend is marked `UNVERIFIED` because `kqueue` (Apple Silicon, M1 Pro) is the only path with measured results so far.

A typical banner on Apple Silicon:

```
SwiftNet runtime: os=macOS (24.5.0) arch=arm64 cores=10 logical/10 physical
  topology: P-cores=8 E-cores=2
  backend: kqueue [VERIFIED]   simd: NEON
  pinning: DISABLED (macOS: KERN_NOT_SUPPORTED)
```

On Linux with a working `io_uring` probe:

```
SwiftNet runtime: os=Linux (6.8.0) arch=x86_64 cores=16 logical/16 physical
  backend: io_uring [UNVERIFIED]   simd: AVX2
  pinning: ENABLED
```

The pinning line varies by outcome:

- `pinning: ENABLED` when supported and permitted.
- `pinning: DISABLED (macOS: KERN_NOT_SUPPORTED)` on macOS.
- `pinning: DISABLED (not permitted by OS/cgroup)` when the affinity probe fails elsewhere (for example, inside a restricted container).

> The `topology:` line is printed only when SwiftNet detects distinct performance and efficiency cores (`P-cores`/`E-cores`), which today is Apple Silicon.

## Common pitfalls

- **Don't look for a flag to force a backend.** There isn't one, by design. If you need `io_uring` on Linux, make sure `liburing` is present at build time and the runtime environment allows the syscall — the probe does the rest.
- **`[UNVERIFIED]` is a benchmark-honesty tag, not a "broken" tag.** It means that backend has not been throughput-measured, not that it fails to work. See [Platform support](../reference/platform-support.md) for the per-backend status, and the benchmarks page for what has actually been measured.
- **A container that "should" have `io_uring` may still get `epoll`.** That is the probe working as intended — seccomp or an older host kernel can deny `io_uring_queue_init`, and SwiftNet falls back rather than failing.
- **macOS will never show `pinning: ENABLED`.** This is a platform limitation, not a SwiftNet setting.

## See also

- [Architecture overview](overview.md)
- [Configuration](../guides/configuration.md)
- [Platform support](../reference/platform-support.md)
