# WebSockets

SwiftNet speaks WebSockets (RFC 6455) on the same coroutine I/O runtime as your HTTP handlers. You register a path with `app.ws(...)`, and a coroutine drives that one connection for its whole lifetime.

## Quick start

```cpp
#include <swiftnet.hpp>
using namespace swiftnet;

int main() {
    SwiftNet app(8080);

    // Echo server: send every message back to the client.
    app.ws("/chat", [](ws::WebSocket s) -> vthread {
        for (auto m = co_await s.recv(); !m.closed; m = co_await s.recv()) {
            co_await s.send_text(m.data);
        }
    });

    app.listen([] { /* listening on :8080 */ });
}
```

The handler is a coroutine. Each `co_await` on `recv()` or a send unmounts the virtual thread while it waits for the socket, so an idle WebSocket holds no OS thread.

## How it works

- `app.ws(path, handler)` registers a session handler for an upgrade request to `path`. The signature is `SwiftNet& ws(const std::string& path, ws::handler_t)` (`include/swiftnet.hpp`), where `ws::handler_t = std::function<vthread(WebSocket)>` (`include/ws/websocket.hpp`).
- The handler receives a `ws::WebSocket` **by value** and returns a `vthread` coroutine. SwiftNet completes the HTTP upgrade handshake for you (computing the `Sec-WebSocket-Accept` header), then runs your coroutine.
- `co_await s.recv()` yields the next complete message. Fragmented frames are reassembled, and control frames are handled inline: an incoming ping is answered with a pong automatically.
- A peer close, EOF, or socket error surfaces as a message with `closed == true`. That is your signal to stop the loop and let the coroutine return.
- Outgoing server frames are sent unmasked; incoming client frames are unmasked for you, so `message.data` is the raw payload.

## The session API

`ws::WebSocket` is the entire per-connection surface. Confirmed against `include/ws/websocket.hpp`:

| Member | Signature | Returns |
| --- | --- | --- |
| `recv()` | `vthread_base<message> recv()` | The next complete `message`. `closed == true` on close/EOF/error. |
| `send_text(data)` | `vthread_base<int> send_text(std::string)` | Bytes written; `< 0` on error. |
| `send_binary(data)` | `vthread_base<int> send_binary(std::string)` | Bytes written; `< 0` on error. |
| `close(code)` | `vthread_base<int> close(std::uint16_t code = 1000)` | Bytes written for the close frame. |
| `fd()` | `int fd() const` | The underlying socket file descriptor. |

Each method returns an awaitable, so call it with `co_await`.

### The `message` type

`recv()` resolves to a `ws::WebSocket::message`:

```cpp
struct message {
    std::string data;     // the (unmasked) payload
    bool binary = false;  // false = text frame, true = binary frame
    bool closed = false;  // true once the peer closed / connection ended
};
```

Check `closed` first; when it is `true`, `data` is empty and you should exit the loop.

## Session lifecycle

1. A client sends an HTTP `Upgrade: websocket` request to your `ws` path.
2. SwiftNet performs the handshake and constructs the `WebSocket`, passing along any bytes already read past the upgrade request so early frames are not lost.
3. Your coroutine runs. It typically loops on `co_await s.recv()` until a message with `closed == true` arrives.
4. You may call `co_await s.close()` to initiate a graceful close, or simply return from the coroutine; either way the connection is torn down when the coroutine finishes.

```cpp
app.ws("/events", [](ws::WebSocket s) -> vthread {
    co_await s.send_text("welcome");

    while (true) {
        auto m = co_await s.recv();
        if (m.closed) break;                 // peer went away

        if (m.binary) {
            co_await s.send_binary(m.data);  // bounce binary frames back
        } else if (m.data == "bye") {
            co_await s.close(1000);          // normal closure
            break;
        } else {
            co_await s.send_text("echo: " + m.data);
        }
    }
});
```

> Receiving a message with `closed == true` means the connection is already gone. Do not call `send_text`/`send_binary` after that point; just exit the loop.

## Common pitfalls

- **Forgetting `co_await`.** `recv()`, `send_text`, `send_binary`, and `close` all return awaitables. Without `co_await` you create the awaitable but never run it (and likely get a compiler diagnostic for the unused coroutine result).
- **Taking the session by reference.** The handler signature is `vthread(WebSocket)` — the `WebSocket` is passed by value and owned by your coroutine. Write `[](ws::WebSocket s) -> vthread`, not `WebSocket&`.
- **Looping forever after a close.** If you ignore `message.closed`, `recv()` keeps returning closed messages and your loop spins. Always break on `closed`.
- **Blocking the engine.** WebSocket handlers run on the I/O runtime. For CPU-heavy work inside a session, offload it so you do not stall the engine — see [async-and-offload.md](async-and-offload.md).
- **Treating binary as text.** `message.binary` tells you the frame type. Reply with `send_binary` for binary payloads and `send_text` for text.

## See also

- [Routing](routing.md) — the HTTP verb routes (`get`, `post`, …) that share the same app and runtime.
- [Async and offload](async-and-offload.md) — coroutine handlers and moving CPU-heavy work off the I/O engine.
