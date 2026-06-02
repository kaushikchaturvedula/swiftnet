# Off-host benchmark kit (move the bottleneck off the harness)

**Why:** on a single machine the load generator (`wrk`) and the server share cores
and talk over loopback, so HTTP throughput tops out at the *harness* ceiling and the
server sits ~92% idle (see the repo `BENCHMARKS.md` §1). To measure SwiftNet's **true
server throughput** you must run `wrk` on a **separate box** over a real network and
**confirm the server is CPU-saturated** (idle% near 0). This kit does exactly that.

> Status: **setup only.** No numbers are produced until you provision the two boxes
> and run it. Nothing here fabricates results.

---

## Topology

```
  ┌────────────────┐        VPC private network         ┌────────────────┐
  │   LOAD box      │  ──────────────────────────────▶  │   SERVER box    │
  │  wrk (≥ server) │      (same AZ + placement group)   │ SwiftNet / Node │
  │  load_box.sh    │  ◀──────────────────────────────  │ / Fastify/Spring│
  └────────────────┘                                     │  server_box.sh  │
                                                          │  + cpu_watch    │
                                                          └────────────────┘
```

- **One server at a time** on the SERVER box (identical hardware for all four → fair).
- Load is driven from the LOAD box at the SERVER box's **private IP**.
- `cpu_watch.sh` on the SERVER box proves saturation.

## Recommended hardware (AWS Graviton / arm64 — matches your dev arch)

| Role | Instance | Why |
|---|---|---|
| **SERVER box** | `c7g.4xlarge` (Graviton3, 16 vCPU, 32 GB) | enough cores to show SwiftNet's per-core scaling; arm64 like your M1 Pro |
| **LOAD box** | `c7g.8xlarge` (32 vCPU) **or 2× c7g.4xlarge** | the load box must be **bigger** than the server, or `wrk` becomes the bottleneck |

- Same **region + AZ**, in a **cluster placement group**, same **VPC/subnet** → lowest, most stable latency.
- Default **ENA** networking (c7g has up to 30 Gbps) — plenty; the goal is to saturate server *CPU*, not the NIC.
- **Security group:** allow the server port (8080/3000/8090) **from the load box's SG/private IP only**.
- Use **private IPs** (same subnet) — never benchmark over a public IP / NAT.
- Newer option: `c8g` (Graviton4) for both, same sizing ratio.
- **Smaller/cheaper smoke test:** `c7g.2xlarge` server + `c7g.4xlarge` load.

### Backend that gets exercised
- **Linux server box (Graviton) → SwiftNet uses io_uring.** These would be the **first
  real-hardware io_uring throughput numbers** (currently *unverified* — see `BENCHMARKS.md`).
  Bare EC2 has no Docker seccomp filter, so io_uring runs natively (unlike the Docker
  functional check). Label results `(linux/arm64, io_uring)`.
- **macOS server box (EC2 mac2.metal, arm64) → kqueue**, the already-measured backend.
  Use this if you want an apples-to-apples extension of the existing kqueue numbers.

---

## One-time setup (both boxes)

```bash
# both boxes: toolchain
sudo apt-get update && sudo apt-get install -y build-essential cmake git wrk \
     liburing-dev nodejs npm openjdk-21-jdk-headless
# (if distro wrk is old/missing: build from https://github.com/wg/wrk)

git clone <your-swiftnet-remote> swiftnet && cd swiftnet

# SERVER box: build SwiftNet (Linux release => io_uring backend auto-selected)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target swiftnet_bench -j
#   competitors (server box):
(cd benchmark && npm install)                        # node + fastify
(cd benchmark/springboot && ./gradlew bootJar)       # spring boot
# NOTE: for Fastify, ensure it listens on host '0.0.0.0' (Fastify defaults to localhost).
```

---

## Run protocol (fair 3-/4-way)

For **each** server S in `swiftnet`, `node` (or `fastify`), `spring`:

```bash
# 1) SERVER box — start S (binds all interfaces, all cores) + saturation monitor:
./benchmark/remote/server_box.sh S
#    (leave it running; it streams busy%/idle%/server_cpu% once per second)

# 2) LOAD box — sweep connections, save raw wrk stdout:
SERVER=<server-private-ip> ./benchmark/remote/load_box.sh S
#    prints PEAK req/s and the connection count where it peaked.

# 3) SERVER box — Ctrl-C server_box.sh → it prints the saturation summary.
```

### The saturation gate (this is the whole point)
At the peak connection count, the `cpu_watch` summary on the server box **must** show
**idle% near 0** and **server_cpu% near (cores×100)%**. Only then is the number a real
*server* throughput. If idle% is still high:
- the **load box is too small / too far** → use a bigger load box, a 2nd load box, or move to the same placement group;
- or the **route is too cheap** to saturate at the offered concurrency → raise `CONNS`.

Re-run until the server is the bottleneck, then the per-server PEAK req/s + p50/p99
(from `--latency`) are directly comparable across the four servers.

### Outputs
- LOAD box: `benchmark/results/offhost-<server>-<stamp>/c<N>.txt` (raw wrk per conn count) + `meta.txt` (PEAK).
- SERVER box: `cpu_watch` summary (paste it next to each server's PEAK).

Report the four PEAKs side by side with their saturation summaries. Label every number
`(arch, OS, backend, server-instance, load-instance)`. Do **not** compare numbers taken
with different load boxes or with idle% still high.

---

## Files
- `server_box.sh` — launch one server on all interfaces/cores + run `cpu_watch`.
- `load_box.sh` — drive wrk from the separate box, sweep connections, save raw output.
- `cpu_watch.sh` — per-second CPU saturation monitor (Linux `/proc`; macOS `top` fallback).
