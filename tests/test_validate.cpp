#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "swiftnet.hpp"

#include <optional>
#include <string>
#include <string_view>

using namespace swiftnet;

// ---- types under validation ----
struct User
{
    std::string name;
    int age{};
    std::string email;
    std::string role;
    std::optional<std::string> nickname;
};
template <>
struct swiftnet::schema<User>
{
    static constexpr auto rules = swiftnet::rules(
        swiftnet::field<&User::name>(swiftnet::required{}, swiftnet::len(1, 50)),
        swiftnet::field<&User::age>(swiftnet::range(0, 150)),
        swiftnet::field<&User::email>(swiftnet::pattern("^[^@]+@[^@]+$")),
        swiftnet::field<&User::role>(swiftnet::one_of("admin", "user", "guest")),
        swiftnet::field<&User::nickname>(swiftnet::required{}, swiftnet::max_len(20)));
};

struct Priority
{
    int level{};
};
template <>
struct swiftnet::schema<Priority>
{
    static constexpr auto rules = swiftnet::rules(
        swiftnet::field<&Priority::level>(swiftnet::one_of(1, 2, 3)));
};

struct Bounds
{
    int n{};
    std::string s;
};
template <>
struct swiftnet::schema<Bounds>
{
    static constexpr auto rules = swiftnet::rules(
        swiftnet::field<&Bounds::n>(swiftnet::min(10), swiftnet::max(20)),
        swiftnet::field<&Bounds::s>(swiftnet::min_len(2)));
};

struct Plain // intentionally NO schema specialization
{
    int x{};
    std::string y;
};

// ---- helpers ----
static Request make_request(const std::string &body)
{
    http::request hr;
    hr.method = "POST";
    hr.path = "/x";
    hr.body = body;
    return Request(hr);
}
static const FieldError *err_for(const std::vector<FieldError> &es, std::string_view field)
{
    for (auto &e : es)
        if (e.field == field)
            return &e;
    return nullptr;
}
static const char *kValidUser =
    R"({"name":"ada","age":30,"email":"ada@example.com","role":"admin","nickname":"a"})";

TEST_CASE("validate: a fully valid payload passes")
{
    auto v = make_request(kValidUser).validate<User>();
    CHECK(v.ok);
    CHECK(v.errors.empty());
    CHECK(v.value.name == "ada");
    CHECK(v.value.age == 30);
}

TEST_CASE("validate: numeric range / min / max")
{
    // range: above hi
    {
        auto v = make_request(R"({"name":"a","age":200,"email":"a@b","role":"user","nickname":"x"})").validate<User>();
        CHECK(!v.ok);
        auto *e = err_for(v.errors, "age");
        REQUIRE(e);
        CHECK(e->rule == "range");
    }
    // range: below lo
    {
        auto v = make_request(R"({"name":"a","age":-1,"email":"a@b","role":"user","nickname":"x"})").validate<User>();
        CHECK(err_for(v.errors, "age"));
    }
    // min / max via Bounds
    {
        auto lo = make_request(R"({"n":5,"s":"ok"})").validate<Bounds>();
        REQUIRE(err_for(lo.errors, "n"));
        CHECK(err_for(lo.errors, "n")->rule == "min");
        auto hi = make_request(R"({"n":99,"s":"ok"})").validate<Bounds>();
        CHECK(err_for(hi.errors, "n")->rule == "max");
        auto ok = make_request(R"({"n":15,"s":"ok"})").validate<Bounds>();
        CHECK(ok.ok);
    }
}

TEST_CASE("validate: string length (len / min_len / max_len)")
{
    // len: empty name fails lower bound
    {
        auto v = make_request(R"({"name":"","age":1,"email":"a@b","role":"user","nickname":"x"})").validate<User>();
        REQUIRE(err_for(v.errors, "name"));
        CHECK(err_for(v.errors, "name")->rule == "length");
    }
    // len: too-long name fails upper bound (51 chars)
    {
        std::string longName(51, 'a');
        auto v = make_request("{\"name\":\"" + longName + "\",\"age\":1,\"email\":\"a@b\",\"role\":\"user\",\"nickname\":\"x\"}").validate<User>();
        CHECK(err_for(v.errors, "name"));
    }
    // min_len via Bounds (s must be >= 2)
    {
        auto v = make_request(R"({"n":15,"s":"x"})").validate<Bounds>();
        REQUIRE(err_for(v.errors, "s"));
        CHECK(err_for(v.errors, "s")->rule == "min_length");
    }
}

TEST_CASE("validate: pattern (regex) match and no-match")
{
    auto bad = make_request(R"({"name":"a","age":1,"email":"not-an-email","role":"user","nickname":"x"})").validate<User>();
    REQUIRE(err_for(bad.errors, "email"));
    CHECK(err_for(bad.errors, "email")->rule == "pattern");

    auto good = make_request(R"({"name":"a","age":1,"email":"a@b","role":"user","nickname":"x"})").validate<User>();
    CHECK(!err_for(good.errors, "email"));
}

TEST_CASE("validate: one_of (string and numeric)")
{
    auto bad = make_request(R"({"name":"a","age":1,"email":"a@b","role":"root","nickname":"x"})").validate<User>();
    REQUIRE(err_for(bad.errors, "role"));
    CHECK(err_for(bad.errors, "role")->rule == "one_of");

    CHECK(make_request(R"({"level":2})").validate<Priority>().ok);
    auto pn = make_request(R"({"level":5})").validate<Priority>();
    REQUIRE(err_for(pn.errors, "level"));
    CHECK(err_for(pn.errors, "level")->rule == "one_of");
}

TEST_CASE("validate: required on optional — present vs absent")
{
    // present -> ok
    {
        auto v = make_request(R"({"name":"a","age":1,"email":"a@b","role":"user","nickname":"bob"})").validate<User>();
        CHECK(!err_for(v.errors, "nickname"));
    }
    // absent -> required fires
    {
        auto v = make_request(R"({"name":"a","age":1,"email":"a@b","role":"user"})").validate<User>();
        REQUIRE(err_for(v.errors, "nickname"));
        CHECK(err_for(v.errors, "nickname")->rule == "required");
    }
}

TEST_CASE("validate: value constraint skipped when optional absent, applied when present")
{
    // absent: ONLY required fires, NOT max_length
    {
        auto v = make_request(R"({"name":"a","age":1,"email":"a@b","role":"user"})").validate<User>();
        auto *e = err_for(v.errors, "nickname");
        REQUIRE(e);
        CHECK(e->rule == "required"); // not "max_length"
    }
    // present but too long: max_length fires
    {
        std::string big(21, 'z');
        auto v = make_request("{\"name\":\"a\",\"age\":1,\"email\":\"a@b\",\"role\":\"user\",\"nickname\":\"" + big + "\"}").validate<User>();
        REQUIRE(err_for(v.errors, "nickname"));
        CHECK(err_for(v.errors, "nickname")->rule == "max_length");
    }
}

TEST_CASE("validate: multiple simultaneous failures are all collected")
{
    auto v = make_request(R"({"name":"a","age":999,"email":"nope","role":"root","nickname":"a"})").validate<User>();
    CHECK(!v.ok);
    CHECK(v.errors.size() == 3); // age(range) + email(pattern) + role(one_of)
    CHECK(err_for(v.errors, "age"));
    CHECK(err_for(v.errors, "email"));
    CHECK(err_for(v.errors, "role"));
}

TEST_CASE("validate: a type with no schema degrades to ok (just parses)")
{
    auto v = make_request(R"({"x":7,"y":"hi"})").validate<Plain>();
    CHECK(v.ok);
    CHECK(v.errors.empty());
    CHECK(v.value.x == 7);
    CHECK(v.value.y == "hi");
}

TEST_CASE("bind: success returns the value and leaves the response untouched")
{
    Response res;
    auto u = make_request(kValidUser).bind<User>(res);
    REQUIRE(u.has_value());
    CHECK(u->name == "ada");
    CHECK(res.status() == 200); // not written
}

TEST_CASE("bind: failure writes a structured 400 JSON and returns nullopt")
{
    Response res;
    auto u = make_request(R"({"name":"a","age":999,"email":"nope","role":"root","nickname":"a"})").bind<User>(res);
    CHECK(!u.has_value());

    http::response out = res.to_http_response();
    CHECK(out.status == 400);
    bool ct = false;
    for (auto &[k, val] : out.headers)
        if (k == "Content-Type" && val.find("application/json") != std::string::npos)
            ct = true;
    CHECK(ct);

    Json j = Json::parse(out.body);
    CHECK(j["error"] == "validation_failed");
    REQUIRE(j["details"].is_array());
    CHECK(j["details"].size() == 3);
    // each detail has field/rule/message
    for (auto &d : j["details"])
    {
        CHECK(d.contains("field"));
        CHECK(d.contains("rule"));
        CHECK(d.contains("message"));
    }
}

TEST_CASE("validate: malformed JSON parses to a DEFAULT-constructed value, then validates")
{
    auto v = make_request("{ this is not valid json ]").validate<User>();
    // Contract: body<T>() swallows the parse error and yields a default-constructed T
    // (this is the inherited behavior the validator runs on top of) -- prove it explicitly:
    CHECK(v.value.name.empty());
    CHECK(v.value.age == 0);
    CHECK(v.value.email.empty());
    CHECK(v.value.role.empty());
    CHECK(!v.value.nickname.has_value());
    // ...and those defaults then violate the schema, so validation reports failure.
    CHECK(!v.ok);
    CHECK(err_for(v.errors, "nickname")); // required fires on the absent optional
    CHECK(err_for(v.errors, "email"));    // empty string fails the email pattern
}
