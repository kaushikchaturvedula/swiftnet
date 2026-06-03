// SwiftNet typed-JSON example (Fastify/FastAPI/Spring-style DX). Built in CI as
// the `typed_json` target. Demonstrates Glaze typed (de)serialization on the hot
// path plus the dynamic Json path, an async handler, and offload().
//
//   curl localhost:8080/users/sample
//   curl -X POST localhost:8080/users -d '{"id":7,"name":"ada","active":true}'
#include "swiftnet.hpp"
#include "vthread_scheduler.hpp" // swiftnet::offload
#include <string>

using namespace swiftnet;

// A plain struct -- Glaze reflects it at compile time; no schema/macros needed.
struct User
{
    int id{};
    std::string name;
    bool active{};
};

int main()
{
    SwiftNet app(8080);

    // Typed serialize: hand a struct straight to res.json (native-speed Glaze).
    app.get("/users/sample", [](Request &, Response &res) {
        User u{1, "ada", true};
        res.json(u);
    });

    // Typed parse: req.body<User>() deserializes the request body into the struct.
    app.post("/users", [](Request &req, Response &res) {
        User u = req.body<User>();
        if (u.name.empty())
        {
            res.bad_request("name is required");
            return;
        }
        res.status(201).json(u);
    });

    // The dynamic Json path is still available for ad-hoc documents.
    app.get("/dyn", [](Request &, Response &res) {
        Json j;
        j["ok"] = true;
        j["items"] = Json::array({1, 2, 3});
        res.json(j);
    });

    // Async handler: offload CPU-heavy work to a stealable compute task so it does
    // not block the engine's I/O-bound connections, then resume and respond.
    app.get("/work", [](Request &, Response &res) -> vthread {
        std::uint64_t acc = 0;
        co_await swiftnet::offload([&acc] {
            for (long i = 0; i < 2'000'000; ++i)
                acc += static_cast<std::uint64_t>(i) * 2654435761u;
        });
        Json j;
        j["acc"] = acc;
        res.json(j);
        co_return;
    });

    app.listen([] { /* listening on :8080 */ });
    return 0;
}
