#ifndef tcp_socket_hpp
#define tcp_socket_hpp

#include "../io_awaitable.hpp"
#include "../vthread.hpp"
#include <fcntl.h>
#include <unistd.h>

namespace swiftnet::net
{

    class tcp_socket
    {
    public:
        explicit tcp_socket(int fd = -1);
        tcp_socket(tcp_socket &&o) noexcept;
        tcp_socket &operator=(tcp_socket &&o) noexcept;
        ~tcp_socket();

        int fd() const { return fd_; }
        void close();

        vthread_base<int> async_read(void *buf, std::size_t len);
        vthread_base<int> async_write(const void *buf, std::size_t len);

    private:
        int fd_;
        void set_nonblock();
    };

}

#endif
