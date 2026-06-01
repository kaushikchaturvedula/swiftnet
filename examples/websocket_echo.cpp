// SwiftNet WebSocket echo server.
//
// Each connection runs as a virtual thread that co_awaits frames from the
// reactor (unmounting between messages) and echoes them back. Test with:
//   websocat ws://127.0.0.1:8082/echo
//   (or any WebSocket client)

#include "swiftnet.hpp"
#include <csignal>
#include <iostream>

using namespace swiftnet;

static SwiftNet *g_app = nullptr;
static void on_sigint(int) { if (g_app) g_app->close(); }

int main()
{
    SwiftNet app(8082);
    g_app = &app;
    std::signal(SIGINT, on_sigint);

    app.get("/", [](Request &, Response &res) {
        res.html("<h1>SwiftNet WebSocket echo</h1>"
                 "<p>Connect a WebSocket client to <code>ws://localhost:8082/echo</code>.</p>");
    });

    // WebSocket echo session: read messages until the peer closes, echo each.
    app.ws("/echo", [](ws::WebSocket socket) -> vthread {
        std::cout << "ws connected (fd=" << socket.fd() << ")\n";
        while (true)
        {
            auto msg = co_await socket.recv();
            if (msg.closed)
                break;
            if (msg.binary)
                co_await socket.send_binary(msg.data);
            else
                co_await socket.send_text("echo: " + msg.data);
        }
        std::cout << "ws closed\n";
        co_return;
    });

    std::cout << "websocket echo on http://localhost:8082  (ws path: /echo)\n"
                 "Press Ctrl+C to stop.\n";
    app.listen();
    return 0;
}
