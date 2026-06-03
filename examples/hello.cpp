// SwiftNet hello-world. Compiles against the current API (built in CI as the
// `hello` target so it can never go stale). Run, then:
//   curl localhost:8080/        -> Hello, World!
//   curl localhost:8080/json    -> {"message":"Hello, World!"}
#include "swiftnet.hpp"

using namespace swiftnet;

int main()
{
    SwiftNet app(8080);

    app.get("/", [](Request &, Response &res) {
        res.text("Hello, World!");
    });

    app.get("/json", [](Request &, Response &res) {
        Json j;
        j["message"] = "Hello, World!";
        res.json(j); // dynamic JSON
    });

    // Blocks until SIGINT/close(); the runtime/config banner is logged at startup.
    app.listen([] { /* listening on :8080 */ });
    return 0;
}
