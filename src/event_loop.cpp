// event_loop façade. The per-backend reactor logic lives in
// src/detail/backend/<name>_backend.cpp; this file only selects one at
// construction. macOS/Windows are a compile-time choice; on Linux the backend is
// chosen at RUNTIME from the cached runtime_info (io_uring when the kernel probe
// succeeds AND liburing is present, else epoll).
#include "event_loop.hpp"
#include "detail/runtime_detect.hpp"
#include <stdexcept>

using namespace swiftnet;

event_loop::event_loop()
{
#if defined(__linux__)
    const auto be = detail::cached_runtime().backend;
#if defined(SWIFTNET_HAS_LIBURING)
    if (be == detail::event_backend::io_uring)
        b_ = detail::make_iouring_backend();
    else
        b_ = detail::make_epoll_backend();
#else
    (void)be; // no liburing compiled: epoll is the only Linux backend available
    b_ = detail::make_epoll_backend();
#endif
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    b_ = detail::make_kqueue_backend();
#elif defined(SWIFTNET_BACKEND_IOCP)
    b_ = detail::make_iocp_backend();
#else
    throw std::runtime_error("no reactor backend compiled for this platform");
#endif
}

event_loop::~event_loop() = default;
