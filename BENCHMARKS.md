# SwiftNet Benchmarks

**What is real here:** every throughput/latency number in this document was measured on
**Apple Silicon (M1 Pro, arm64) with the kqueue backend**, and each is reproducible with the
scripts in `benchmark/`. The Linux (io_uring) and Windows (IOCP) backends are **implemented but
NOT performance-measured** — see [Backend status](#backend-status). No speed is claimed for them.

> The headline finding of this round is methodological and matters more than any single number:
> **on this one machine the HTTP throughput ceiling (~100–118K req/s) is set by the co-located load
> generator and the loopback TCP path, not by SwiftNet.** The server sits ~92% idle at that rate and
> the number does not change whether it runs on 2 cores or 10. Single-host throughput comparisons
> therefore measure the *harness*, not the server. The defensible server metrics on this box are
> **per-request latency** and **CPU-per-request**, both reported below.

## Test environment

| | |
|---|---|
| **Machine** | Apple M1 Pro, 10 cores (8 performance + 2 efficiency), 16 GB |
| **OS** | macOS 26 (Darwin 25.5), **kqueue** I/O backend |
| **Build** | Apple clang 21, C++20, `-O3`, native arm64 (`-DCMAKE_OSX_ARCHITECTURES=arm64`) |
| **Load generator** | [`wrk`](https://github.com/wg/wrk) 4.2.0 (kqueue), HTTP/1.1 keep-alive, **same host** |
| **Server under test** | `benchmark/swiftnet_bench` — a *minimal* server (no logging/CORS/body middleware) so wrk measures framework cost, not app code |

Routes: `GET /` (plaintext "Hello, World!"), `GET /json` (`{"message":"Hello, World!"}`),
`GET /user/:id` (router param + small JSON).

---

## 1. The throughput ceiling is the harness, not the server

`benchmark/ceiling_sweep.sh` sweeps (server engines) × (client load) on the 10-core box, `/json`:

```
A) 10 engines, increasing client load:
   wrk -t2  -c200   => 118197 req/s @ 1.68 ms   <- peak, low concurrency
   wrk -t4  -c500   => 102658 req/s @ 4.85 ms
   wrk -t8  -c500   =>  99598 req/s @ 4.96 ms
   wrk -t12 -c1000  => 102165 req/s @ 9.61 ms   <- 5x the connections, ~same RPS, 6x the latency
B) match server+client to 10 cores (no oversubscription):
   5 engines => 102550   6 engines => 101578   4 engines => 99048
C) scale engines under a fixed heavy client:
   2 engines =>  99886   4 engines => 101187   8 engines => 100125   <- 2 engines == 8 engines
```

Two signatures of a saturated *pipe* rather than a saturated *server*:
1. RPS is flat (~100K) while latency rises linearly with connections (Little's law: `RPS ≈ conns / latency`).
2. **Engine count is irrelevant** — 2 engines serve as much as 8. A CPU-bound server would scale with cores; this one doesn't, because it isn't CPU-bound.

This is corroborated by the on-CPU profile (next section): at the ceiling the server is **92% idle**.
The practical consequence: **server-side throughput optimizations cannot raise the measured RPS on
this single-machine loopback setup.** A real server-vs-server throughput comparison needs an
**off-host load generator**, which this environment does not have.

---

## 2. The real server metrics (these *are* measurable here)

### 2a. Per-request latency — `wrk -t1 -c1` (one connection, no queueing)

| Route | p50 | p75 | p90 | p99 | avg |
|---|--:|--:|--:|--:|--:|
| `GET /` (plaintext) | **25 µs** | 28 µs | 33 µs | 64 µs | 27 µs |
| `GET /json` | **26 µs** | 30 µs | 36 µs | 104 µs | — |

25 µs median round-trip over loopback (accept→parse→route→serialize→write→read on the client)
is the honest "how fast is one request" number. This is where the per-core engine model pays off
(a connection is pinned to one engine and never hops threads).

### 2b. CPU-per-request — server sampled at the ~118K ceiling (`wrk -t2 -c200`)

`sample`d the server process for 6 s while serving 116,647 req/s (`benchmark/results/efficiency-*`):

```
total leaf samples : 55879
idle (kevent/cvwait): 51410  (92.0%)   <- engine threads parked waiting for I/O
ON-CPU             :  4469  ( 8.0%)
   read   (syscall): 1949  ┐ 82% of all on-CPU time is the unavoidable
   write  (syscall): 1728  ┘ one read + one write per request
   malloc family   :  ~230  (libmalloc tiny/free)
   nlohmann json   :   ~55  (dump/dump_escaped/destroy)
   std::string     :   ~45  (append/push_back, mostly inside json dump)
   hash tables     :   ~15  (io_ops slot + Request headers)
   router/route    :   ~14
```

Reading: at the ceiling the server burns ~8% of the 10 cores ≈ **~7 µs CPU per request**, and
**82% of that is the two socket syscalls**. Everything allocation/serialization-related is the
remaining ~1.4% of total CPU. This is *why* the allocation experiments below barely move the needle
on this machine — there is almost no allocation CPU left to remove relative to syscalls and idle wait.

---

## 3. Optimization experiments (each measured; kept only if real)

Per the directive "keep only what shows a real, reproducible gain on macOS/kqueue; report negatives honestly."

### 3a. Coroutine-frame pool — **NEGATIVE / neutral → OFF by default on Apple**

Gate-A (on an earlier logging server) ranked coroutine-frame `malloc/free` high, so I added a
thread-local frame pool (`include/detail/frame_pool.hpp`) overriding the vthread `promise_type`
`operator new/delete`. A/B (`benchmark/ab_framepool.sh`, 3 reps, interleaved):

```
                  /  (plaintext)                 /json
vector-backed  pool ~2% SLOWER             pool ~2% SLOWER
intrusive-list nopool 107906/106302/107022 nopool  99938/101726/101033
               pool   107538/106269/109311 pool   101563/103062/102787
               => ~neutral (+0.6%)         => +1.6% (within run-to-run noise)
```

Conclusion: macOS `libmalloc`'s per-thread magazine already serves same-size, same-thread frames
near-optimally; a custom pool can't beat it and the bookkeeping is pure overhead. **Disabled by
default on Apple** (`SWIFTNET_USE_FRAME_POOL` gate in `vthread.hpp`); the lean version is kept ON
for non-Apple platforms (e.g. glibc, where the default allocator is weaker for this pattern) but is
**UNVERIFIED** there. Force either way with `-DSWIFTNET_FORCE_FRAME_POOL` / `-DSWIFTNET_NO_FRAME_POOL`.

### 3b. Lazy request headers — **POSITIVE (kept)**

The `Request` constructor used to copy *every* request header into an `unordered_map` per request,
even though most handlers never read a header. Now `Request` holds a pointer to the parser's header
vector and `header()` scans it on demand; the map is built only if `headers()` is called
(`src/swiftnet.cpp`). A/B (`benchmark/ab_lazy_headers.sh`, 3 reps, `/json`):

```
minimal headers (wrk default ~4):  eager 101003/99729/97041   lazy 100946/95996/95088   (neutral; drift)
10 headers/request (realistic):    eager  92286/88144/91445   lazy  97035/94894/96342   (+6%, no overlap)
```

The win scales with header count (eager = hash table + N nodes + 2N string copies). With minimal
headers it's in the noise; with a realistic 10 headers it's a clean, reproducible **+6%**, and it is
algorithmically *strictly less work* for handlers that don't read headers, so it cannot regress the
common path. Kept.

### 3c. Per-core engine scheduler — kept (latency/correctness, not throughput)

Each core is one pinned thread fusing reactor + run-queue + its own `SO_REUSEPORT` listener;
a connection's coroutine, fds and pending I/O are engine-local, so the per-request I/O path takes
**no locks**. This is the source of the 25 µs p50 latency and is ThreadSanitizer/AddressSanitizer
clean under load. It is **throughput-neutral on this box** (the box is harness-bound — see §1), so it
is presented as a latency/correctness/scalability change, not a throughput win.

### 3d. Build: native CPU tuning + a real bug fix

Fixed a pre-existing bug — the project gated compiler flags on `STREQUAL "Clang"`, which never
matches Apple's `AppleClang`, so `-Wall -Wextra` (and any native tuning) were silently dropped on
macOS. Now `MATCHES "Clang"`. Native tuning (`-mcpu=native` arm64 / `-march=native` x86) is added
**only if the selected toolchain accepts it** (`SWIFTNET_NATIVE`, default ON); Apple's CommandLineTools
`c++` rejects it under cross-arch so it is safely skipped (point CMake at `/usr/bin/clang++` to enable
it). Optional `-DSWIFTNET_LTO=ON`. None of these move the harness-bound number on this box.

---

## Backend status

| Backend | Platform | Status | Evidence |
|---|---|---|---|
| **kqueue** | macOS/BSD | ✅ **VERIFIED — measured** | Everything above (Apple Silicon, arm64) |
| **io_uring** | Linux | ⚙️ Implemented, **functionally verified, throughput UNMEASURED** | Compiles + `ctest` 2/2 pass + live `GET /json` round-trip in Docker `ubuntu:24.04` linux/arm64, kernel 6.12 (`benchmark/results/linux-iouring-verify-*`). Readiness-based (`io_uring_prep_poll_add`); does **not** yet use multishot / provided buffers / `DEFER_TASKRUN`. Docker's default seccomp blocks io_uring (`io_uring_setup`/`enter`), so it needs `--security-opt seccomp=unconfined` — a real deployment caveat. **No speed claim.** |
| **IOCP** | Windows | 🚧 **Skeleton — UNVERIFIED** | `arm()` readiness is a no-op, timers use a throwaway thread, completion result is a placeholder. Needs real `OVERLAPPED` + `WSARecv/WSASend` integration. Compiles as a shape only. **No speed claim.** |

Backend status is also documented at the top of `include/event_loop.hpp`.

---

## Threats to validity

- **Single-host load (dominant).** `wrk` runs on the same 10 cores as the server over loopback. §1
  shows this — not the server — sets the ~100–118K ceiling. Absolute throughput would be higher with
  a dedicated load box; **latency (§2a) and CPU-per-request (§2b) are the host-independent metrics.**
- **Workload shape.** Tiny CPU-light responses stress framework overhead. A workload with real async
  I/O (DB calls) would shift the picture toward whichever runtime unmounts most cheaply.
- **Apple Silicon / kqueue only.** io_uring and IOCP are not performance-measured (see above).
- **No cross-framework throughput claim is made here.** Earlier 3-way runs vs Node/Spring Boot were
  taken on a *logging* SwiftNet server (≈70K) and are not comparable to these minimal-server numbers;
  re-running Node (cluster) and Spring Boot against the minimal server on a **dedicated load box** is
  the right way to compare servers, and is left as future work because this host cannot do it fairly.

---

## Reproduce

```bash
# Build (native arm64 release; minimal benchmark server target = swiftnet_bench)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"
cmake --build build --target swiftnet_bench -j

# §1  Why the ceiling is the harness (engines × client load sweep)
./benchmark/ceiling_sweep.sh

# §2  Latency + CPU-per-request
build/swiftnet_bench 8080 &                       # then:
wrk -t1 -c1  -d8s  --latency http://127.0.0.1:8080/json     # per-request latency
wrk -t2 -c200 -d12s        http://127.0.0.1:8080/json &     # drive ~118K and
sample $(pgrep swiftnet_bench) 6                            # sample on-CPU fraction

# §3a frame-pool A/B   (needs build-pool with -DSWIFTNET_FORCE_FRAME_POOL)
./benchmark/ab_framepool.sh
# §3b lazy-headers A/B (needs /tmp/bench_eager_headers = pre-change binary)
./benchmark/ab_lazy_headers.sh

# Linux io_uring functional check (compiles + runs; NOT a speed test)
docker run --rm --security-opt seccomp=unconfined -v "$PWD":/src:ro ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq build-essential cmake git liburing-dev >/dev/null
  cp -r /src /w && cd /w && cmake -S . -B b -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build b -j4 >/dev/null && ctest --test-dir b --output-on-failure'
```

Raw `wrk`/`sample` output for every run above is saved under `benchmark/results/` and
`benchmark/profiles/`.
