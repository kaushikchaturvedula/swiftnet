# Installation

This page covers what you need to build SwiftNet, the build and test commands, and how to consume SwiftNet from your own CMake project.

## Prerequisites

| Requirement | Minimum | Notes |
|---|---|---|
| C++ compiler | Clang >= 17 / GCC >= 13 | C++23 is required (Glaze, the typed-JSON path, needs it). Tested on Apple Clang 21. |
| CMake | >= 3.20 | Configure + build driver. |
| Git | any recent | Used by CMake `FetchContent` to pull dependencies that aren't already installed. |

SwiftNet builds as a static library (`swiftnet`). The `CMAKE_CXX_STANDARD` is fixed to `23` with extensions off, so a compiler without complete C++23 support will fail to configure.

## Dependencies

You do not install these by hand. CMake first tries `find_package`, and only falls back to fetching a pinned version if the package isn't found on your system.

| Dependency | Used for | How it's provided |
|---|---|---|
| `nlohmann/json` | Dynamic `Json` documents (`res.json(Json{...})`, `req.json()`) | `find_package`, else fetched (`v3.11.3`) |
| `glaze` | Compile-time typed JSON (`res.json(struct)`, `req.body<T>()`) | `find_package`, else fetched (`v7.7.1`) |
| `spdlog` | Logging | `find_package`, else fetched (`v1.15.3`) |
| `rapidyaml` | YAML config file parsing | Vendored (single header in `third_party/rapidyaml/`) |
| `doctest` | Unit tests | `find_package`, else fetched (`v2.4.11`); only when `SWIFTNET_BUILD_TESTS=ON` |
| `liburing` | Linux `io_uring` backend | Found at build time with `find_library`; if absent, the build uses `epoll` |

> On Linux, `liburing` is optional. When it isn't found, SwiftNet compiles without the `io_uring` probe and auto-detects `epoll` at runtime instead. See [I/O backend auto-detection](../architecture/auto-detection.md).

## Quick start

Clone the repository, then configure, build, and test:

```bash
git clone https://github.com/kaushikchaturvedula/swiftnet.git
cd swiftnet

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The first configure may take a moment while CMake fetches any dependencies it can't find locally. The example programs (including `hello`) build by default, so you can run one immediately:

```bash
./build/examples/hello
```

> The examples and tests are on by default. Turn them off with `-DSWIFTNET_BUILD_EXAMPLES=OFF` and `-DSWIFTNET_BUILD_TESTS=OFF` if you only want the library.

## Apple Silicon (arm64)

On a fresh build directory on Apple Silicon, pass the architecture and SDK explicitly. This pins a native arm64 build and points CMake at a real SDK path (a stale cached SDK can make even standard headers fail to resolve):

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

> ⚠️ Always configure into a *clean* build directory when changing these flags. CMake caches `CMAKE_OSX_ARCHITECTURES` and `CMAKE_OSX_SYSROOT`; reusing a directory that was configured for x86_64 (e.g. under Rosetta) or with a now-missing SDK can produce confusing build failures. Delete `build/` and reconfigure.

## CMake build options

These are all `cmake -D<NAME>=<VALUE>` flags.

| Option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | (unset) | Use `Release` for optimized builds. |
| `SWIFTNET_NATIVE` | `ON` | Tune for the host CPU (`-mcpu=native` on arm64, `-march=native` on x86). Auto-skipped if the toolchain rejects the flag. |
| `SWIFTNET_LTO` | `OFF` | Link-time optimization. Slower builds; below the measurement floor on the loopback benchmark. |
| `SWIFTNET_SANITIZE` | `none` | One of `none`, `address`, `thread`, `undefined`. ASan and TSan are mutually exclusive. |
| `SWIFTNET_BUILD_EXAMPLES` | `ON` | Build the programs in `examples/`. |
| `SWIFTNET_BUILD_TESTS` | `ON` | Build the doctest suite and register it with `ctest`. |

Sanitizer builds use separate directories, for example:

```bash
cmake -S . -B build-tsan -DSWIFTNET_SANITIZE=thread  && cmake --build build-tsan -j
cmake -S . -B build-asan -DSWIFTNET_SANITIZE=address && cmake --build build-asan -j
```

## How it works

- **`find_package` first, fetch as fallback.** If `nlohmann/json`, `glaze`, `spdlog`, or `doctest` are already installed, CMake uses them; otherwise it fetches the pinned version listed above. `rapidyaml` is always vendored.
- **The I/O backend is chosen at runtime, not at install time.** The platform macro (`SWIFTNET_BACKEND_KQUEUE` / `IOURING` / `IOCP`) is set by CMake from `CMAKE_SYSTEM_NAME`, but the *actual* backend, SIMD path, and core-pinning are detected and logged when the server starts. See [auto-detection](../architecture/auto-detection.md) and [platform support](../reference/platform-support.md).
- **`swiftnet` is a static library** with public include directory `include/`. Linking it gives you the public headers (`swiftnet.hpp`, `schema.hpp`, `scope.hpp`, `json.hpp`) and propagates `glaze` as a public dependency; `nlohmann/json` and `spdlog` are private.

## Consuming SwiftNet from your project

### Option A — `add_subdirectory`

Vendor or submodule SwiftNet into your tree, then add it and link `swiftnet`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/swiftnet)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE swiftnet)
```

If you only want the library, disable the bundled examples and tests before adding it:

```cmake
set(SWIFTNET_BUILD_EXAMPLES OFF)
set(SWIFTNET_BUILD_TESTS OFF)
add_subdirectory(third_party/swiftnet)
```

### Option B — `FetchContent`

Let CMake pull SwiftNet at configure time:

```cmake
include(FetchContent)
FetchContent_Declare(
    swiftnet
    GIT_REPOSITORY https://github.com/kaushikchaturvedula/swiftnet.git
    GIT_TAG main
)
FetchContent_MakeAvailable(swiftnet)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE swiftnet)
```

Either way, linking `swiftnet` is all you need — its public include directory is on your target automatically, so `#include "swiftnet.hpp"` just works:

```cpp
#include "swiftnet.hpp"
using namespace swiftnet;

int main() {
    SwiftNet app(8080);
    app.get("/", [](Request&, Response& res) { res.text("Hello, World!"); });
    app.listen([] { /* listening on :8080 */ });
}
```

## Common pitfalls

- **Reusing a build directory after changing arch/SDK flags.** `CMAKE_OSX_ARCHITECTURES` and `CMAKE_OSX_SYSROOT` are cached. Delete `build/` and reconfigure when you change them, or you may keep building for the wrong architecture or against a missing SDK.
- **A pre-C++23 compiler.** Configuration fails because the standard is required, not optional. Verify with `clang++ --version` / `g++ --version` and ensure you meet Clang >= 17 / GCC >= 13.
- **Expecting `io_uring` without `liburing`.** On Linux the `io_uring` probe is only compiled in when `liburing` is found at build time. Install your distro's `liburing` development package (for example `liburing-dev`) before configuring if you want it.
- **Mixing sanitizers.** `address` and `thread` cannot be combined. Use separate build directories for each.
- **Forgetting `find_package` precedence.** If you have an old system-installed `spdlog` or `glaze`, CMake will prefer it over the pinned fetch. Remove or shadow the system package if you hit a version-related build error.

## See also

- [Your first server](first-server.md) — build and run a minimal SwiftNet app.
- [Configuration](../guides/configuration.md) — runtime knobs via code, YAML, and environment variables.
- [Auto-detection](../architecture/auto-detection.md) — how the I/O backend, SIMD path, and pinning are chosen.
- [Platform support](../reference/platform-support.md) — per-platform backend status.
