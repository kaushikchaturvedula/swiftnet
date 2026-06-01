#ifndef swiftnet_ws_websocket_hpp
#define swiftnet_ws_websocket_hpp

#include "../net/tcp_socket.hpp"
#include "../vthread.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace swiftnet::ws
{

    // A WebSocket connection (RFC 6455), built on the coroutine I/O runtime.
    // recv()/send_*() co_await the reactor, so a WebSocket session unmounts its
    // virtual thread while waiting for frames -- just like an HTTP handler.
    //
    // Server semantics: incoming (client) frames are masked and are unmasked
    // here; outgoing (server) frames are sent unmasked. Control frames are
    // handled inline: a ping is answered with a pong; a close is surfaced as a
    // closed message (after echoing a close frame).
    class WebSocket
    {
    public:
        struct message
        {
            std::string data;
            bool binary = false; // false = text frame
            bool closed = false; // true once the peer closed / connection ended
        };

        // `initial` carries any bytes already read past the HTTP upgrade request
        // (so early WebSocket frames are not lost).
        explicit WebSocket(net::tcp_socket sock, std::string initial = {})
            : sock_(std::move(sock)), rbuf_(std::move(initial)) {}

        // Receive the next complete message (reassembling fragments, answering
        // pings). On close/EOF/error returns a message with closed == true.
        vthread_base<message> recv();

        // Send a text or binary message. Returns bytes written (<0 on error).
        vthread_base<int> send_text(std::string data);
        vthread_base<int> send_binary(std::string data);

        // Send a close frame.
        vthread_base<int> close(std::uint16_t code = 1000);

        int fd() const { return sock_.fd(); }

        // ---- Handshake helper (used by the http layer) ----
        // Compute the Sec-WebSocket-Accept value for a client's
        // Sec-WebSocket-Key (base64(sha1(key + magic GUID))).
        static std::string accept_key(const std::string &client_key);

    private:
        vthread_base<bool> ensure(std::size_t n); // ensure rbuf_ has >= n bytes
        vthread_base<int> send_frame(std::uint8_t opcode, const std::string &data);

        net::tcp_socket sock_;
        std::string rbuf_; // unconsumed bytes already read from the socket
    };

    // A WebSocket session handler: a coroutine driving one connection.
    using handler_t = std::function<vthread(WebSocket)>;

} // namespace swiftnet::ws

#endif
