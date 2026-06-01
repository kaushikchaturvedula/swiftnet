#include "detail/os_backend.hpp"
#include <string>
#include <sstream>

#ifdef SWIFTNET_PLATFORM_WINDOWS
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "mswsock.lib")
#endif

namespace swiftnet::detail::platform
{
    void init_networking()
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed: " + std::to_string(result));
        }
#endif
        // No initialization needed for Unix-like systems
    }

    void cleanup_networking()
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        WSACleanup();
#endif
        // No cleanup needed for Unix-like systems
    }

    int make_socket_nonblocking(int fd)
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        u_long mode = 1;
        return ioctlsocket(fd, FIONBIO, &mode);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return flags;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    int close_socket(int fd)
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        return closesocket(fd);
#else
        return close(fd);
#endif
    }

    int platform_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen)
    {
#ifdef SWIFTNET_PLATFORM_MACOS
        // macOS doesn't have accept4, use regular accept
        return accept(listen_fd, addr, addrlen);
#elif defined(SWIFTNET_PLATFORM_LINUX)
        // Linux has accept4 which can set SOCK_NONBLOCK atomically
        return accept4(listen_fd, addr, addrlen, SOCK_NONBLOCK);
#elif defined(SWIFTNET_PLATFORM_WINDOWS)
        return accept(listen_fd, addr, addrlen);
#else
        return accept(listen_fd, addr, addrlen);
#endif
    }

    int get_last_socket_error()
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    std::string get_error_string(int error_code)
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        char* msg = nullptr;
        FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error_code,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&msg),
            0,
            nullptr
        );
        
        std::string result;
        if (msg) {
            result = msg;
            LocalFree(msg);
            // Remove trailing newline if present
            if (!result.empty() && result.back() == '\n') {
                result.pop_back();
                if (!result.empty() && result.back() == '\r') {
                    result.pop_back();
                }
            }
        } else {
            result = "Unknown error: " + std::to_string(error_code);
        }
        return result;
#else
        return std::strerror(error_code);
#endif
    }
} 