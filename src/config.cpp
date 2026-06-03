#include "config.hpp"
#include "detail/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

// rapidyaml: this is the ONE translation unit that defines the amalgamated impl.
#define RYML_SINGLE_HDR_DEFINE_NOW
#include "ryml_all.hpp"

namespace swiftnet
{
    namespace
    {
        // ---- small parse helpers (defensive; never throw to the caller) ----
        bool parse_bool(const std::string &s, bool dflt) noexcept
        {
            std::string v;
            for (char c : s)
                v += static_cast<char>(std::tolower((unsigned char)c));
            if (v == "1" || v == "true" || v == "t" || v == "yes" || v == "on")
                return true;
            if (v == "0" || v == "false" || v == "f" || v == "no" || v == "off")
                return false;
            return dflt;
        }
        long long parse_ll(const std::string &s, long long dflt) noexcept
        {
            try { return std::stoll(s); }
            catch (...) { return dflt; }
        }

        std::optional<std::string> env(const char *name) noexcept
        {
            const char *v = std::getenv(name);
            if (!v || !*v)
                return std::nullopt;
            return std::string(v);
        }

        // ---- YAML overlay (rapidyaml); malformed -> logged + ignored ----
        std::optional<std::string> read_file(const std::string &path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
                return std::nullopt;
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }

        [[noreturn]] void ryml_error(const char *msg, std::size_t len, ryml::Location, void *)
        {
            throw std::runtime_error(std::string(msg, len));
        }

        // Fetch a scalar value by key as a string (so we reuse the parse helpers and
        // avoid ryml type-conversion quirks). Returns nullopt if absent/not scalar.
        std::optional<std::string> yaml_scalar(ryml::ConstNodeRef root, const char *key)
        {
            if (!root.is_map() || !root.has_child(ryml::to_csubstr(key)))
                return std::nullopt;
            ryml::ConstNodeRef n = root[ryml::to_csubstr(key)];
            if (!n.has_val())
                return std::nullopt;
            ryml::csubstr v = n.val();
            return std::string(v.str, v.len);
        }

        void overlay_yaml(Config &c)
        {
            std::string path;
            if (auto p = env("SWIFTNET_CONFIG"))
                path = *p;
            else
                path = "swiftnet.yaml";
            auto text = read_file(path);
            if (!text)
                return; // no file: skip silently (YAML is optional)

            ryml::Callbacks cb = ryml::get_callbacks();
            cb.m_error = &ryml_error; // throw instead of abort on malformed input
            ryml::set_callbacks(cb);
            try
            {
                ryml::Tree t = ryml::parse_in_arena(ryml::to_csubstr(*text));
                ryml::ConstNodeRef root = t.rootref();
                if (auto v = yaml_scalar(root, "port")) c.port = static_cast<std::uint16_t>(parse_ll(*v, c.port));
                if (auto v = yaml_scalar(root, "engines")) c.engines = static_cast<std::size_t>(parse_ll(*v, (long long)c.engines));
                if (auto v = yaml_scalar(root, "backlog")) c.backlog = static_cast<int>(parse_ll(*v, c.backlog));
                if (auto v = yaml_scalar(root, "steal")) c.steal = parse_bool(*v, c.steal);
                if (auto v = yaml_scalar(root, "steal_threshold")) c.steal_threshold = static_cast<int>(parse_ll(*v, c.steal_threshold));
                if (auto v = yaml_scalar(root, "steal_max_batch")) c.steal_max_batch = static_cast<int>(parse_ll(*v, c.steal_max_batch));
                if (auto v = yaml_scalar(root, "steal_min_idle")) c.steal_min_idle = static_cast<int>(parse_ll(*v, c.steal_min_idle));
                if (auto v = yaml_scalar(root, "max_header_bytes")) c.max_header_bytes = static_cast<std::size_t>(parse_ll(*v, (long long)c.max_header_bytes));
                if (auto v = yaml_scalar(root, "max_body_bytes")) c.max_body_bytes = static_cast<std::size_t>(parse_ll(*v, (long long)c.max_body_bytes));
                if (auto v = yaml_scalar(root, "log_level")) c.log_level = *v;
                if (auto v = yaml_scalar(root, "iouring_provided_buffers")) c.iouring_provided_buffers = parse_bool(*v, c.iouring_provided_buffers);
            }
            catch (const std::exception &e)
            {
                SWIFTNET_LOG_WARN("config: ignoring malformed YAML at {}: {}", path, e.what());
            }
            ryml::set_callbacks(ryml::get_callbacks()); // restore defaults
        }

        void overlay_env(Config &c)
        {
            if (auto v = env("SWIFTNET_PORT")) c.port = static_cast<std::uint16_t>(parse_ll(*v, c.port));
            if (auto v = env("SWIFTNET_ENGINES")) c.engines = static_cast<std::size_t>(parse_ll(*v, (long long)c.engines));
            if (auto v = env("SWIFTNET_BACKLOG")) c.backlog = static_cast<int>(parse_ll(*v, c.backlog));
            if (auto v = env("SWIFTNET_STEAL")) c.steal = parse_bool(*v, c.steal);
            if (auto v = env("SWIFTNET_STEAL_THRESHOLD")) c.steal_threshold = static_cast<int>(parse_ll(*v, c.steal_threshold));
            if (auto v = env("SWIFTNET_STEAL_MAX_BATCH")) c.steal_max_batch = static_cast<int>(parse_ll(*v, c.steal_max_batch));
            if (auto v = env("SWIFTNET_STEAL_MIN_IDLE")) c.steal_min_idle = static_cast<int>(parse_ll(*v, c.steal_min_idle));
            if (auto v = env("SWIFTNET_MAX_HEADER_BYTES")) c.max_header_bytes = static_cast<std::size_t>(parse_ll(*v, (long long)c.max_header_bytes));
            if (auto v = env("SWIFTNET_MAX_BODY_BYTES")) c.max_body_bytes = static_cast<std::size_t>(parse_ll(*v, (long long)c.max_body_bytes));
            if (auto v = env("SWIFTNET_LOG_LEVEL")) c.log_level = *v;
            if (auto v = env("SWIFTNET_IOURING_PROVIDED_BUFFERS")) c.iouring_provided_buffers = parse_bool(*v, c.iouring_provided_buffers);
        }

        template <typename T>
        T clamp_range(T v, T lo, T hi) { return std::max(lo, std::min(v, hi)); }

        void clamp(Config &c)
        {
            if (c.port == 0) c.port = 8080;
            const std::size_t lc = c.detected.logical_cores ? c.detected.logical_cores : 1;
            if (c.engines > lc) c.engines = lc; // 0 (=all) left as-is
            c.backlog = clamp_range(c.backlog, 1, 1 << 20);
            c.steal_threshold = clamp_range(c.steal_threshold, 0, 1 << 20);
            c.steal_max_batch = clamp_range(c.steal_max_batch, 1, 1 << 16);
            const int eng = static_cast<int>(c.resolved_engines());
            c.steal_min_idle = clamp_range(c.steal_min_idle, 0, eng);
            c.max_header_bytes = clamp_range<std::size_t>(c.max_header_bytes, 1024, 1u << 20);
            c.max_body_bytes = clamp_range<std::size_t>(c.max_body_bytes, 0, 1ull << 31);
            static const char *levels[] = {"trace", "debug", "info", "warn", "error"};
            if (std::none_of(std::begin(levels), std::end(levels),
                             [&](const char *l) { return c.log_level == l; }))
                c.log_level = "info";
        }

        void apply_log_level(const std::string &lvl)
        {
            spdlog::level::level_enum e = spdlog::level::info;
            if (lvl == "trace") e = spdlog::level::trace;
            else if (lvl == "debug") e = spdlog::level::debug;
            else if (lvl == "warn") e = spdlog::level::warn;
            else if (lvl == "error") e = spdlog::level::err;
            spdlog::set_level(e);
        }
    } // namespace

    Config load_config(Config base)
    {
        Config c = base;                  // built-in defaults, optionally pre-seeded by code
        c.detected = detail::cached_runtime(); // need logical_cores for clamping engines
        overlay_yaml(c);                  // defaults/code -> YAML
        overlay_env(c);                   // -> env (always wins)
        clamp(c);                         // valid ranges
        apply_log_level(c.log_level);
        return c;
    }

    void log_config(const Config &c) noexcept
    {
        SWIFTNET_LOG_INFO("SwiftNet config: engines={} port={} backlog={}",
                          c.resolved_engines(), c.port, c.backlog);
        SWIFTNET_LOG_INFO("  valve: {} (threshold={} max_batch={} min_idle={})",
                          c.steal ? "ON" : "OFF", c.steal_threshold, c.steal_max_batch, c.steal_min_idle);
        SWIFTNET_LOG_INFO("  limits: max_header={}B max_body={}B   log_level={}",
                          c.max_header_bytes, c.max_body_bytes, c.log_level);
    }

} // namespace swiftnet
