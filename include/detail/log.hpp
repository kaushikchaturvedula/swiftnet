#ifndef swiftnet_detail_log_hpp
#define swiftnet_detail_log_hpp

// Lightweight logging facade over spdlog for SwiftNet internal components.
//
// Low-level components (scheduler, reactor, net, http) cannot include the
// top-level <swiftnet.hpp> without creating a circular dependency, so they log
// through these macros instead. They forward to spdlog's default logger and
// honour SPDLOG_ACTIVE_LEVEL, so TRACE/DEBUG calls compile to nothing in a
// release build (SPDLOG_ACTIVE_LEVEL is set per build type in CMakeLists.txt).
//
// Use fmt-style format strings, e.g.:
//     SWIFTNET_LOG_DEBUG("accept fd={} errno={}", fd, errno);

#include <spdlog/spdlog.h>

#define SWIFTNET_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define SWIFTNET_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define SWIFTNET_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define SWIFTNET_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define SWIFTNET_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)

#endif // swiftnet_detail_log_hpp
