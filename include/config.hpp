#pragma once
// Layered configuration: built-in defaults -> programmatic (code) -> YAML file ->
// environment variables. ENV ALWAYS WINS. Every knob has a default, a range, and
// is documented in the README config reference. The "embedded" choices (I/O
// backend, SIMD path, core-pinning) are NOT here -- they are auto-detected and not
// overridable (see detail/runtime_detect.hpp); `detected` exposes them read-only.

#include "detail/runtime_detect.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace swiftnet
{

    struct Config
    {
        // ---- user-tunable knobs (defaults shown) ----
        std::uint16_t port = 8080;            // SWIFTNET_PORT        / yaml: port
        std::size_t engines = 0;              // SWIFTNET_ENGINES     / engines  (0 => all logical cores)
        int backlog = 1024;                   // SWIFTNET_BACKLOG     / backlog
        bool steal = false;                   // SWIFTNET_STEAL       / steal    (work-stealing valve; default OFF)
        int steal_threshold = 1;              // SWIFTNET_STEAL_THRESHOLD / steal_threshold
        int steal_max_batch = 1;              // SWIFTNET_STEAL_MAX_BATCH / steal_max_batch
        int steal_min_idle = 0;               // SWIFTNET_STEAL_MIN_IDLE  / steal_min_idle
        std::size_t max_header_bytes = 64 * 1024;       // SWIFTNET_MAX_HEADER_BYTES / max_header_bytes
        std::size_t max_body_bytes = 8 * 1024 * 1024;   // SWIFTNET_MAX_BODY_BYTES   / max_body_bytes
        std::string log_level = "info";       // SWIFTNET_LOG_LEVEL   / log_level (trace|debug|info|warn|error)
        bool iouring_provided_buffers = false; // SWIFTNET_IOURING_PROVIDED_BUFFERS (Linux; reserved, UNVERIFIED)

        // ---- embedded, auto-detected (read-only; not overridable) ----
        detail::runtime_info detected;

        // Resolved engine count (engines, or all logical cores when engines==0).
        std::size_t resolved_engines() const noexcept
        {
            return engines ? engines : (detected.logical_cores ? detected.logical_cores : 1);
        }
    };

    // Produce the effective config: start from `base` (built-in defaults, optionally
    // pre-seeded with programmatic values), overlay the YAML file (path from
    // SWIFTNET_CONFIG, else ./swiftnet.yaml if present), then overlay environment
    // variables (which always win), clamp to valid ranges, and fill `detected`.
    // YAML is optional: absent -> skipped; malformed -> logged + ignored (never throws).
    Config load_config(Config base = {});

    // Log the resolved config block at startup (engines/port/backlog/valve/limits).
    void log_config(const Config &) noexcept;

} // namespace swiftnet
