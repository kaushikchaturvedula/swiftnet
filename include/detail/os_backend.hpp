#pragma once

// Platform detection and backend selection
#if defined(_WIN32) || defined(_WIN64)
    #ifndef SWIFTNET_BACKEND_IOCP
        #define SWIFTNET_BACKEND_IOCP 1
    #endif
    #define SWIFTNET_PLATFORM_WINDOWS 1
    
    // Windows-specific includes
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <mswsock.h>
    
#elif defined(__APPLE__) && defined(__MACH__)
    #ifndef SWIFTNET_BACKEND_KQUEUE
        #define SWIFTNET_BACKEND_KQUEUE 1
    #endif
    #define SWIFTNET_PLATFORM_MACOS 1
    
    // macOS-specific includes
    #include <sys/event.h>
    #include <sys/time.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    
#elif defined(__linux__)
    #ifndef SWIFTNET_BACKEND_IOURING
        #define SWIFTNET_BACKEND_IOURING 1
    #endif
    #define SWIFTNET_PLATFORM_LINUX 1
    
    // Linux-specific includes
    #include <sys/epoll.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    
    // Optional liburing support
    #ifdef SWIFTNET_HAS_LIBURING
        #include <liburing.h>
    #endif
    
#else
    // Default to epoll for other Unix-like systems
    #ifndef SWIFTNET_BACKEND_EPOLL
        #define SWIFTNET_BACKEND_EPOLL 1
    #endif
    #define SWIFTNET_PLATFORM_UNIX 1
    
    #include <sys/epoll.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

// Common includes
#include <cstring>
#include <cerrno>
#include <string>

// Platform-specific socket utilities
namespace swiftnet::detail::platform 
{
    // Cross-platform socket initialization
    void init_networking();
    void cleanup_networking();
    
    // Cross-platform socket utilities
    int make_socket_nonblocking(int fd);
    int close_socket(int fd);
    
    // Platform-specific accept function
    int platform_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen);
    
    // Error handling
    int get_last_socket_error();
    std::string get_error_string(int error_code);
}
