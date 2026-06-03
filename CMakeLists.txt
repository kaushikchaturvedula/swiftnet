cmake_minimum_required(VERSION 3.20)
project(SwiftNet VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard. C++23 is required by Glaze (the fast typed-JSON path); clang/gcc
# support it fully and it is a superset of the C++20 the coroutine/pmr code used.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SWIFTNET_PLATFORM_LINUX TRUE)
    add_compile_definitions(SWIFTNET_BACKEND_IOURING=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(SWIFTNET_PLATFORM_MACOS TRUE)
    add_compile_definitions(SWIFTNET_BACKEND_KQUEUE=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SWIFTNET_PLATFORM_WINDOWS TRUE)
    add_compile_definitions(SWIFTNET_BACKEND_IOCP=1)
endif()

# Compiler-specific flags
include(CheckCXXCompilerFlag)
# Native-CPU tuning (ON by default). Apple clang on arm64 does not accept
# -march=native, so pick the right per-platform flag and only add it if the
# compiler actually accepts it. NOTE: on a loopback benchmark this is below the
# measurement floor (the server is I/O-wait-bound; see BENCHMARKS.md), but it is
# the correct way to let the optimizer use the host's ISA (NEON/AdvSIMD on ARM,
# AVX etc. on x86) and matters on a CPU-bound deployment.
# NOTE: match "Clang" with MATCHES so Apple's "AppleClang" id is included --
# STREQUAL "Clang" silently skipped this whole block (and -Wall) on macOS.
option(SWIFTNET_NATIVE "Tune for the host CPU (-mcpu/-march=native)" ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O3")
    if(SWIFTNET_NATIVE)
        # Probe the actually-selected compiler/arch. arm64 wants -mcpu=native, x86
        # wants -march=native; only add whichever the toolchain accepts so the build
        # never breaks. (Apple's CommandLineTools `c++` rejects both under cross-arch;
        # pointing CMake at /usr/bin/clang++ enables -mcpu=native on Apple Silicon.)
        check_cxx_compiler_flag("-mcpu=native" SWIFTNET_HAS_MCPU_NATIVE)
        check_cxx_compiler_flag("-march=native" SWIFTNET_HAS_MARCH_NATIVE)
        if(SWIFTNET_HAS_MCPU_NATIVE)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=native")
        elseif(SWIFTNET_HAS_MARCH_NATIVE)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
        endif()
    endif()
    message(STATUS "SwiftNet CXX flags: ${CMAKE_CXX_FLAGS} (proc=${CMAKE_SYSTEM_PROCESSOR} osx=${CMAKE_OSX_ARCHITECTURES})")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /O2")
endif()

# Optional link-time optimization (OFF by default: slower builds, and below the
# measurement floor on the loopback benchmark). Enable with -DSWIFTNET_LTO=ON.
option(SWIFTNET_LTO "Enable link-time optimization (IPO)" OFF)
if(SWIFTNET_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT SWIFTNET_IPO_OK OUTPUT SWIFTNET_IPO_MSG)
    if(SWIFTNET_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO requested but not supported: ${SWIFTNET_IPO_MSG}")
    endif()
endif()

# Optional sanitizer build. AddressSanitizer and ThreadSanitizer are mutually
# exclusive, so this is a single string choice rather than independent flags.
# Used to gate the concurrency-critical stages (reactor / park-before-arm):
#   cmake -S . -B build-tsan -DSWIFTNET_SANITIZE=thread
#   cmake -S . -B build-asan -DSWIFTNET_SANITIZE=address
set(SWIFTNET_SANITIZE "none" CACHE STRING "Sanitizer: none | address | thread | undefined")
set_property(CACHE SWIFTNET_SANITIZE PROPERTY STRINGS none address thread undefined)
if(SWIFTNET_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
elseif(SWIFTNET_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
elseif(SWIFTNET_SANITIZE STREQUAL "undefined")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

# Compile out TRACE/DEBUG logging in optimized builds; keep it for debug/sanitizer
# builds. SPDLOG_ACTIVE_LEVEL must be defined before <spdlog/spdlog.h> is included,
# so it is set globally here.
if(SWIFTNET_SANITIZE STREQUAL "none" AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
else()
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
endif()

# Find required packages
find_package(Threads REQUIRED)

# Find nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Find spdlog for logging
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.3
    )
    FetchContent_MakeAvailable(spdlog)
endif()

# Glaze: header-only, compile-time-reflection JSON for the typed fast path
# (res.json(struct) / req.body<T>()). Requires C++23. nlohmann::json is kept for
# dynamic/convenience JSON. Pinned for reproducibility + clang21 compatibility.
find_package(glaze QUIET)
if(NOT glaze_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v7.7.1
    )
    FetchContent_MakeAvailable(glaze)
endif()

# rapidyaml for the YAML config file. Vendored as the official amalgamated single
# header (third_party/rapidyaml/ryml_all.hpp) to avoid the c4core submodule +
# separate-library build; exactly one TU (src/config.cpp) defines
# RYML_SINGLE_HDR_DEFINE_NOW. See third_party/rapidyaml/README.md.
set(SWIFTNET_RYML_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidyaml)

# Platform-specific libraries
if(SWIFTNET_PLATFORM_LINUX)
    # Try to find liburing for Linux
    find_library(LIBURING_LIBRARY NAMES uring)
    if(LIBURING_LIBRARY)
        add_compile_definitions(SWIFTNET_HAS_LIBURING=1)
    endif()
elseif(SWIFTNET_PLATFORM_WINDOWS)
    # Windows-specific libraries
    set(PLATFORM_LIBS ws2_32 wsock32)
endif()

# SwiftNet library sources
set(SWIFTNET_SOURCES
    src/swiftnet.cpp
    src/config.cpp
    src/vthread.cpp
    src/vthread_scheduler.cpp
    src/io_awaitable.cpp
    src/event_loop.cpp
    src/detail/backend/kqueue_backend.cpp
    src/detail/backend/iouring_backend.cpp
    src/detail/backend/epoll_backend.cpp
    src/detail/backend/iocp_backend.cpp
    src/net/tcp_socket.cpp
    src/net/acceptor.cpp
    src/http/http_server.cpp
    src/ws/websocket.cpp
    src/detail/platform_utils.cpp
    src/detail/runtime_detect.cpp
)

# SwiftNet library headers
set(SWIFTNET_HEADERS
    include/swiftnet.hpp
    include/config.hpp
    include/json.hpp
    include/schema.hpp
    include/detail/validate.hpp
    include/scope.hpp
    include/detail/simd.hpp
    include/vthread.hpp
    include/vthread_scheduler.hpp
    include/io_awaitable.hpp
    include/event_loop.hpp
    include/net/tcp_socket.hpp
    include/net/acceptor.hpp
    include/http/http_server.hpp
    include/ws/websocket.hpp
    include/detail/os_backend.hpp
    include/detail/backend/iface.hpp
    include/detail/runtime_detect.hpp
    include/detail/mpsc_queue.hpp
    include/detail/work_queue.hpp
    include/detail/log.hpp
    include/detail/cpu_affinity.hpp
)

# Create the SwiftNet library
add_library(swiftnet STATIC ${SWIFTNET_SOURCES} ${SWIFTNET_HEADERS})

# Set include directories
target_include_directories(swiftnet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${SWIFTNET_RYML_INCLUDE}   # vendored rapidyaml, used only by src/config.cpp
)

# Link libraries
target_link_libraries(swiftnet
    PUBLIC
        Threads::Threads
        glaze::glaze            # header-only typed JSON; used in public headers (json.hpp)
    PRIVATE
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

# Platform-specific linking
if(SWIFTNET_PLATFORM_LINUX AND LIBURING_LIBRARY)
    target_link_libraries(swiftnet PUBLIC ${LIBURING_LIBRARY})
elseif(SWIFTNET_PLATFORM_WINDOWS)
    target_link_libraries(swiftnet PUBLIC ${PLATFORM_LIBS})
endif()

# Enable examples
option(SWIFTNET_BUILD_EXAMPLES "Build SwiftNet examples" ON)
if(SWIFTNET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Minimal benchmark server (no logging middleware; isolates framework cost)
add_executable(swiftnet_bench benchmark/swiftnet_bench.cpp)
target_link_libraries(swiftnet_bench PRIVATE swiftnet nlohmann_json::nlohmann_json spdlog::spdlog)
target_include_directories(swiftnet_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Tests (doctest + ctest)
option(SWIFTNET_BUILD_TESTS "Build SwiftNet tests" ON)
if(SWIFTNET_BUILD_TESTS)
    find_package(doctest QUIET)
    if(NOT doctest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            doctest
            GIT_REPOSITORY https://github.com/doctest/doctest.git
            GIT_TAG v2.4.11
        )
        FetchContent_MakeAvailable(doctest)
    endif()
    enable_testing()
    add_subdirectory(tests)
endif()

# Future: Add installation and packaging configuration
