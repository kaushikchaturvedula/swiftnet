# Configuration

SwiftNet reads its runtime settings from layered sources so you can keep defaults in code, ship a `swiftnet.yaml` with your service, and override anything per-deployment with environment variables.

## Quick start

The simplest configuration is none at all — every knob has a working default:

```cpp
#include <swiftnet.hpp>
using namespace swiftnet;

int main() {
    SwiftNet app;                 // port 8080, engines = all logical cores
    app.get("/", [](Request&, Response& res) {
        res.text("hello");
    });
    app.listen([] {});            // resolves config, logs the effective block, then serves
    return 0;
}
```

Override at launch without touching the binary:

```bash
SWIFTNET_PORT=9090 SWIFTNET_ENGINES=4 SWIFTNET_LOG_LEVEL=debug ./myapp
```

## How it works

Configuration is resolved once, at startup, by `swiftnet::load_config` (see `src/config.cpp`). Sources are applied lowest-to-highest priority, then every value is clamped to its valid range:

- **Built-in defaults** — the field initializers on `swiftnet::Config` (`include/config.hpp`).
- **Programmatic (code)** — what you set in C++. This layer covers **only three knobs**: `port`, `engines`, and `backlog`, via the `SwiftNet(uint16_t port)` constructor, `set_threads(n)`, and `set_backlog(b)`. There is no code path to set the other knobs; use YAML or env for those.
- **YAML file** — overlays any keys present in the file (see resolution below).
- **Environment variables** — applied last, so **environment always wins**.

After overlaying, out-of-range values are clamped (never rejected), an unknown `log_level` falls back to `info`, and `port == 0` is reset to `8080`. The effective block is logged at startup by `log_config`.

> Each layer overlays only the keys it actually provides. A YAML file with just `port:` changes the port and leaves every other knob at its lower-priority value.

### Programmatic layer

```cpp
SwiftNet app(9090);     // port
app.set_threads(4);     // engines (I/O worker count)
app.set_backlog(2048);  // listen() accept backlog
```

> ⚠️ These three programmatic values are still overridden by YAML and environment variables. If `SWIFTNET_PORT` is set, the constructor argument loses.

### YAML file resolution

The YAML path is `$SWIFTNET_CONFIG` if that variable is set, otherwise `./swiftnet.yaml` relative to the working directory.

- A **missing** file is skipped silently — YAML is optional.
- A **malformed** file is logged at `warn` and ignored; it never crashes the server, and the lower layers stand.
- Only scalar keys at the document root are read; unknown keys are ignored.

## Knob reference

| Knob | Env var | YAML key | Default | Range (after clamp) | Platform |
|---|---|---|---|---|---|
| Engine count | `SWIFTNET_ENGINES` | `engines` | `0` (= all logical cores) | `0`, or `1..logical_cores` | all |
| Listen port | `SWIFTNET_PORT` | `port` | `8080` | `1..65535` | all |
| Accept backlog | `SWIFTNET_BACKLOG` | `backlog` | `1024` | `1..1048576` | all |
| Work-steal valve | `SWIFTNET_STEAL` | `steal` | `off` | `0`/`1` | all |
| Steal threshold | `SWIFTNET_STEAL_THRESHOLD` | `steal_threshold` | `1` | `0..1048576` | all |
| Steal max batch | `SWIFTNET_STEAL_MAX_BATCH` | `steal_max_batch` | `1` | `1..65536` | all |
| Min idle before steal | `SWIFTNET_STEAL_MIN_IDLE` | `steal_min_idle` | `0` | `0..engines` | all |
| Max header bytes | `SWIFTNET_MAX_HEADER_BYTES` | `max_header_bytes` | `65536` (64 KiB) | `1024..1048576` | all |
| Max body bytes | `SWIFTNET_MAX_BODY_BYTES` | `max_body_bytes` | `8388608` (8 MiB) | `0..2147483648` | all |
| Log level | `SWIFTNET_LOG_LEVEL` | `log_level` | `info` | `trace`\|`debug`\|`info`\|`warn`\|`error` | all |
| io_uring provided buffers | `SWIFTNET_IOURING_PROVIDED_BUFFERS` | `iouring_provided_buffers` | `off` | `0`/`1` | Linux (reserved, UNVERIFIED) |

Notes on a few entries:

- `engines = 0` means "use all logical cores" and is left as `0` after clamping (it is resolved to the core count at use time). Any non-zero value above the logical core count is clamped down to the core count.
- `steal_min_idle` is clamped against the *resolved* engine count, so it scales with the machine.
- `iouring_provided_buffers` is a reserved Linux knob and is **UNVERIFIED**; it has no effect on other platforms.

The work-stealing valve and its `steal_*` knobs are off by default. See [Work-stealing valve](../architecture/work-stealing-valve.md) for what they do and when to turn them on.

### Boolean parsing

Boolean knobs (`steal`, `iouring_provided_buffers`) accept, case-insensitively: `1`/`true`/`t`/`yes`/`on` for true and `0`/`false`/`f`/`no`/`off` for false. Anything else keeps the lower-priority value.

## Example `swiftnet.yaml`

```yaml
# Resolved from $SWIFTNET_CONFIG, else ./swiftnet.yaml
port: 8080
engines: 0            # 0 = all logical cores
backlog: 1024

# work-stealing valve (off by default)
steal: false
steal_threshold: 1
steal_max_batch: 1
steal_min_idle: 0

# request limits
max_header_bytes: 65536      # 64 KiB
max_body_bytes: 8388608      # 8 MiB

log_level: info
```

Every key is optional; include only the ones you want to override.

## Auto-detected, not configurable

The **I/O backend** (io_uring / epoll / kqueue / IOCP), the **SIMD path** (NEON / AVX2 / SSE2 / scalar), and **core-pinning** are *not* knobs. They are auto-detected at startup, embedded for the run, and logged with a `VERIFIED`/`UNVERIFIED` tag. There is no environment variable or YAML key to force them.

> Only the macOS/kqueue backend is tagged `VERIFIED`. See [Auto-detection](../architecture/auto-detection.md) and [Platform support](../reference/platform-support.md) for the full status of each backend and platform.

## Common pitfalls

- **Expecting code to override env.** It is the other way around: `set_threads`, `set_backlog`, and the constructor port are the *lowest* mutable layer; YAML beats them and environment beats YAML.
- **Trying to set non-core knobs in code.** Only `port`, `engines`, and `backlog` have a programmatic path. For `max_body_bytes`, `log_level`, the `steal_*` knobs, etc., use YAML or environment variables.
- **Out-of-range values "not taking".** Values are clamped, not rejected. `SWIFTNET_BACKLOG=0` becomes `1`; `max_header_bytes=10` becomes `1024`. Check the logged config block to see what actually took effect.
- **Empty environment variables.** An env var set to the empty string is treated as unset and the lower layer wins.
- **Wrong `swiftnet.yaml` location.** Without `$SWIFTNET_CONFIG`, the file is looked up relative to the process working directory, not the binary's directory.
- **Typo in `log_level`.** An unrecognized level silently falls back to `info`.
- **Trying to force a backend.** The I/O backend and SIMD path cannot be overridden; if you need a different one, that is a platform/build concern, not configuration.

## See also

- [Auto-detection](../architecture/auto-detection.md)
- [Work-stealing valve](../architecture/work-stealing-valve.md)
- [Platform support](../reference/platform-support.md)
