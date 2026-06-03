#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "swiftnet.hpp"

using namespace swiftnet;

struct User
{
    int id{};
    std::string name;
    bool active{};
};

static Request make_request(const std::string &body)
{
    http::request hr;
    hr.method = "POST";
    hr.path = "/users";
    hr.body = body;
    return Request(hr);
}

TEST_CASE("glaze: Response::json(struct) serializes via Glaze, valid JSON")
{
    User u{42, "ada", true};
    Response res;
    res.json(u);
    http::response out = res.to_http_response();
    // Body must be valid JSON with the struct's fields.
    Json parsed = Json::parse(out.body);
    CHECK(parsed["id"] == 42);
    CHECK(parsed["name"] == "ada");
    CHECK(parsed["active"] == true);
    // Content-Type set to application/json.
    bool ct = false;
    for (auto &[k, v] : out.headers)
        if (k == "Content-Type" && v.find("application/json") != std::string::npos)
            ct = true;
    CHECK(ct);
}

TEST_CASE("glaze: Request::body<T>() parses into the struct")
{
    Request req = make_request(R"({"id":7,"name":"bob","active":false})");
    User u = req.body<User>();
    CHECK(u.id == 7);
    CHECK(u.name == "bob");
    CHECK(u.active == false);
}

TEST_CASE("glaze: malformed body yields a default-constructed T (no throw)")
{
    Request req = make_request("{ this is not valid json ]");
    User u = req.body<User>();
    CHECK(u.id == 0);
    CHECK(u.name.empty());
    CHECK(u.active == false);
}

TEST_CASE("glaze: round-trip struct -> response body -> struct")
{
    User in{99, "carol", true};
    Response res;
    res.json(in);
    Request req = make_request(res.to_http_response().body);
    User out = req.body<User>();
    CHECK(out.id == in.id);
    CHECK(out.name == in.name);
    CHECK(out.active == in.active);
}

TEST_CASE("glaze: the dynamic Json overload still binds (no ambiguity)")
{
    Json j;
    j["message"] = "hi";
    j["n"] = 3;
    Response res;
    res.json(j); // must select Response::json(const Json&), not the template
    Json parsed = Json::parse(res.to_http_response().body);
    CHECK(parsed["message"] == "hi");
    CHECK(parsed["n"] == 3);
}
