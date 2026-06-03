#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace swiftnet;

namespace
{
    // Clear every knob env var so each case starts from a known state, and point
    // SWIFTNET_CONFIG at a path that does not exist (so no stray ./swiftnet.yaml
    // is picked up) unless the test sets one.
    void clear_env()
    {
        const char *vars[] = {
            "SWIFTNET_PORT", "SWIFTNET_ENGINES", "SWIFTNET_BACKLOG", "SWIFTNET_STEAL",
            "SWIFTNET_STEAL_THRESHOLD", "SWIFTNET_STEAL_MAX_BATCH", "SWIFTNET_STEAL_MIN_IDLE",
            "SWIFTNET_MAX_HEADER_BYTES", "SWIFTNET_MAX_BODY_BYTES", "SWIFTNET_LOG_LEVEL",
            "SWIFTNET_IOURING_PROVIDED_BUFFERS", "SWIFTNET_CONFIG"};
        for (const char *v : vars)
            ::unsetenv(v);
        ::setenv("SWIFTNET_CONFIG", "/swiftnet/definitely/does/not/exist.yaml", 1);
    }

    std::string write_temp_yaml(const std::string &name, const std::string &contents)
    {
        std::string path = std::string("/tmp/") + name;
        std::ofstream(path, std::ios::trunc) << contents;
        return path;
    }
}

TEST_CASE("config: built-in defaults when nothing is set")
{
    clear_env();
    Config c = load_config();
    CHECK(c.port == 8080);
    CHECK(c.backlog == 1024);
    CHECK(c.steal == false);
    CHECK(c.steal_threshold == 1);
    CHECK(c.steal_max_batch == 1);
    CHECK(c.steal_min_idle == 0);
    CHECK(c.log_level == "info");
    CHECK(c.resolved_engines() >= 1); // engines==0 => all logical cores
}

TEST_CASE("config: programmatic base is the starting point")
{
    clear_env();
    Config base;
    base.port = 9999;
    base.steal = true;
    base.backlog = 77;
    Config c = load_config(base);
    CHECK(c.port == 9999);
    CHECK(c.steal == true);
    CHECK(c.backlog == 77);
}

TEST_CASE("config: YAML overrides defaults/base")
{
    clear_env();
    std::string p = write_temp_yaml("swiftnet_test_a.yaml",
                                    "port: 7000\nengines: 2\nsteal: true\nlog_level: warn\nbacklog: 256\n");
    ::setenv("SWIFTNET_CONFIG", p.c_str(), 1);
    Config c = load_config();
    CHECK(c.port == 7000);
    CHECK(c.engines == 2);
    CHECK(c.steal == true);
    CHECK(c.backlog == 256);
    CHECK(c.log_level == "warn");
    std::remove(p.c_str());
}

TEST_CASE("config: env ALWAYS wins over YAML")
{
    clear_env();
    std::string p = write_temp_yaml("swiftnet_test_b.yaml",
                                    "port: 3333\nengines: 5\nsteal: false\n");
    ::setenv("SWIFTNET_CONFIG", p.c_str(), 1);
    ::setenv("SWIFTNET_PORT", "2222", 1);
    ::setenv("SWIFTNET_STEAL", "1", 1);
    Config c = load_config();
    CHECK(c.port == 2222);   // env wins over yaml's 3333
    CHECK(c.steal == true);  // env wins over yaml's false
    CHECK(c.engines == 5);   // only set in yaml -> yaml value
    std::remove(p.c_str());
}

TEST_CASE("config: legacy env names still work")
{
    clear_env();
    ::setenv("SWIFTNET_STEAL", "true", 1);
    ::setenv("SWIFTNET_STEAL_THRESHOLD", "4", 1);
    Config c = load_config();
    CHECK(c.steal == true);
    CHECK(c.steal_threshold == 4);
}

TEST_CASE("config: values are clamped to valid ranges")
{
    clear_env();
    ::setenv("SWIFTNET_BACKLOG", "0", 1);            // -> >= 1
    ::setenv("SWIFTNET_STEAL_THRESHOLD", "-5", 1);   // -> >= 0
    ::setenv("SWIFTNET_STEAL_MAX_BATCH", "0", 1);    // -> >= 1
    ::setenv("SWIFTNET_MAX_HEADER_BYTES", "10", 1);  // -> >= 1 KiB
    ::setenv("SWIFTNET_ENGINES", "100000", 1);       // -> <= logical cores
    ::setenv("SWIFTNET_LOG_LEVEL", "bogus", 1);      // -> "info"
    Config c = load_config();
    CHECK(c.backlog >= 1);
    CHECK(c.steal_threshold == 0);
    CHECK(c.steal_max_batch >= 1);
    CHECK(c.max_header_bytes >= 1024);
    CHECK(c.engines <= c.detected.logical_cores);
    CHECK(c.log_level == "info");
}

TEST_CASE("config: malformed YAML is ignored, never crashes")
{
    clear_env();
    std::string p = write_temp_yaml("swiftnet_test_bad.yaml",
                                    "this: : : not [valid yaml\n  - ][\n\tbad indent\n");
    ::setenv("SWIFTNET_CONFIG", p.c_str(), 1);
    ::setenv("SWIFTNET_PORT", "4444", 1); // env still applies after YAML is dropped
    Config c = load_config();
    CHECK(c.port == 4444);     // didn't crash; env honored
    CHECK(c.backlog == 1024);  // fell back to default
    std::remove(p.c_str());
}
