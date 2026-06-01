#include "net/acceptor.hpp"
#include "io_awaitable.hpp"
#include "vthread_scheduler.hpp"
#include "detail/os_backend.hpp"
#include "detail/log.hpp"
#include <cstring>

#ifdef SWIFTNET_PLATFORM_WINDOWS
    #include <poll.h>
#else
    #include <errno.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

using namespace swiftnet::net;

acceptor::acceptor(uint16_t port, int backlog)
{
    // Initialize networking on Windows
    detail::platform::init_networking();

#ifdef SWIFTNET_PLATFORM_WINDOWS
    listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
#endif

    if (listen_fd_ < 0)
        throw std::runtime_error("socket creation failed");

    // Set non-blocking so accept() returns EAGAIN and we can suspend on it.
    set_nonblock(listen_fd_);

    int opt = 1;
#ifdef SWIFTNET_PLATFORM_WINDOWS
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    #ifdef SO_REUSEPORT
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    #endif
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        detail::platform::close_socket(listen_fd_);
        throw std::runtime_error("bind failed: " + detail::platform::get_error_string(detail::platform::get_last_socket_error()));
    }

    if (listen(listen_fd_, backlog) < 0)
    {
        detail::platform::close_socket(listen_fd_);
        throw std::runtime_error("listen failed: " + detail::platform::get_error_string(detail::platform::get_last_socket_error()));
    }

    SWIFTNET_LOG_INFO("acceptor listening on port {} (fd={}, backlog={})", port, listen_fd_, backlog);
}

acceptor::~acceptor()
{
    detail::platform::close_socket(listen_fd_);
    detail::platform::cleanup_networking();
}

void acceptor::set_nonblock(int fd)
{
    detail::platform::make_socket_nonblocking(fd);
}

swiftnet::vthread acceptor::async_accept(std::function<void(tcp_socket)> cb)
{
    while (true)
    {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);

        int client_fd = detail::platform::platform_accept(listen_fd_, (sockaddr *)&addr, &len);

        if (client_fd >= 0)
        {
            SWIFTNET_LOG_DEBUG("accepted connection client_fd={}", client_fd);
            set_nonblock(client_fd);
            cb(tcp_socket(client_fd));
            continue;
        }

#ifdef SWIFTNET_PLATFORM_WINDOWS
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
        {
            // No connection pending: unmount this virtual thread until the
            // listening socket is readable again. The reactor re-arms a
            // one-shot readability watch and re-mounts us on completion.
            co_await io_awaitable(listen_fd_, POLLIN);
            continue;
        }
        else
        {
            SWIFTNET_LOG_ERROR("accept error: {}", detail::platform::get_error_string(detail::platform::get_last_socket_error()));
            co_return;
        }
    }
}
