#include "net/tcp_socket.hpp"
#include "io_awaitable.hpp"
#include "detail/os_backend.hpp"
#include <cstring>
#include <errno.h>

#ifdef SWIFTNET_PLATFORM_WINDOWS
    #include <poll.h>
#else
    #include <poll.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <sys/socket.h>
#endif

using namespace swiftnet::net;

tcp_socket::tcp_socket(int fd) : fd_(fd)
{
    if (fd_ != -1)
    {
        set_nonblock();
        // Disable Nagle's algorithm: HTTP responses are small and latency-
        // sensitive, and Nagle + delayed-ACK otherwise adds tens of ms.
        int one = 1;
#ifdef SWIFTNET_PLATFORM_WINDOWS
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#else
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
    }
}

tcp_socket::tcp_socket(tcp_socket &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

tcp_socket &tcp_socket::operator=(tcp_socket &&o) noexcept
{
    if (this != &o)
    {
        close();
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

tcp_socket::~tcp_socket() { close(); }

void tcp_socket::set_nonblock()
{
    detail::platform::make_socket_nonblocking(fd_);
}

void tcp_socket::close()
{
    if (fd_ != -1)
    {
        detail::platform::close_socket(fd_);
        fd_ = -1;
    }
}

swiftnet::vthread_base<int> tcp_socket::async_read(void *buf, std::size_t len)
{
    // "Read some": return as soon as any data is available (up to len), rather
    // than blocking until the whole buffer fills. Returns bytes read (>0), 0 on
    // peer close (EOF), or -1 on error; suspends on EAGAIN until readable.
    while (true)
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        ssize_t r = recv(fd_, (char *)buf, len, 0);
#else
        ssize_t r = ::read(fd_, (char *)buf, len);
#endif
        if (r >= 0)
            co_return static_cast<int>(r);

#ifdef SWIFTNET_PLATFORM_WINDOWS
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
            co_await io_awaitable(fd_, POLLIN);
        else
            co_return -1;
    }
}

swiftnet::vthread_base<int> tcp_socket::async_write(const void *buf, std::size_t len)
{
    std::size_t written = 0;
    while (written < len)
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        ssize_t w = send(fd_, (const char *)buf + written, len - written, 0);
#else
        ssize_t w = ::write(fd_, (const char *)buf + written, len - written);
#endif
        if (w > 0)
        {
            written += w;
            continue;
        }
        
#ifdef SWIFTNET_PLATFORM_WINDOWS
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
            co_await io_awaitable(fd_, POLLOUT);
        else
            co_return -1;
    }
    co_return static_cast<int>(written);
}
