# Typed JSON

SwiftNet can serialize and parse JSON straight to and from your own C++ structs. You define a plain struct, and [Glaze](https://github.com/stephenberry/glaze) reflects its fields at compile time — no macros, no schema declaration, no runtime DOM.

## Quick start

```cpp
#include <swiftnet.hpp>
using namespace swiftnet;

// A plain aggregate. Glaze reflects these fields for you at compile time.
struct User {
    int id{};
    std::string name;
    bool active{};
};

int main() {
    SwiftNet app;

    // Serialize a struct to a JSON response body.
    app.get("/user", [](Request& req, Response& res) {
        User u{42, "Ada", true};
        res.json(u);   // -> {"id":42,"name":"Ada","active":true}
    });

    // Parse a JSON request body into a struct.
    app.post("/user", [](Request& req, Response& res) {
        User u = req.body<User>();
        res.status(201).json(u);
    });

    app.listen([] { /* ready */ });
}
```

## How it works

- **No macros, no registration.** Define a plain aggregate struct and Glaze reflects its public fields by name at compile time. There is nothing to annotate.
- **Serialize with `res.json(struct)`.** The `Response::json<T>` template (`include/swiftnet.hpp` ~144) calls `glz::write_json` directly into the response body and sets `Content-Type: application/json`. No intermediate DOM is built.
- **Parse with `req.body<Struct>()`.** The `Request::body<T>` template (`include/swiftnet.hpp` ~78) calls `glz::read_json` into a default-constructed `T`.
- **Field names map directly.** A struct member `name` matches the JSON key `"name"`. Initialize members with `{}` (e.g. `int id{};`) so unspecified fields start at a known default.
- **The dynamic path still exists.** `res.json(Json{...})` and `req.json()` use nlohmann's runtime DOM, side by side with the typed path (see [API variants](#api-variants)).

## Parsing never throws — you must check the result

This is the most important behavior to internalize. `req.body<T>()` does **not** throw and does **not** report errors. On a malformed, missing, or incompatible body it returns a **default-constructed `T`**:

```cpp
// include/swiftnet.hpp ~78
template <class T>
T body() const {
    T out{};
    auto ec = glz::read_json(out, body_);
    if (ec)
        return T{};   // parse failed -> default-constructed value, no throw
    return out;
}
```

So an empty body, garbage bytes, or a JSON object missing the fields you expect all yield the same thing as a perfectly valid request that happened to send zeros and empty strings. There is no way to tell them apart from the return value alone.

> ⚠️ Treat `req.body<T>()` as "best-effort parse, then trust nothing." A default-constructed `User` (`id == 0`, empty `name`, `active == false`) is what you get for a 400-worthy request. Always check the fields you care about, or use validation.

```cpp
app.post("/user", [](Request& req, Response& res) {
    User u = req.body<User>();
    if (u.id == 0 || u.name.empty()) {
        res.bad_request("id and name are required");
        return;
    }
    res.status(201).json(u);
});
```

Writing those checks by hand for every field gets tedious and easy to get wrong — which is exactly why SwiftNet has opt-in validation. See [Validation](validation.md) for declaring constraints once and getting a structured `400` for free via `req.bind<T>(res)`.

## API variants

Typed and dynamic JSON live side by side. The typed `json<T>` overload is constrained by the `GlazeSerializable` concept (`include/json.hpp`) so it never competes with the dynamic `json(const Json&)` overload or with `text()`:

```cpp
// include/json.hpp ~27
template <class T>
concept GlazeSerializable =
    !std::same_as<std::remove_cvref_t<T>, Json> &&        // not nlohmann::json
    !std::convertible_to<const T&, std::string_view> &&   // not a string -> text()
    requires(const T& v, std::string& buf) { glz::write_json(v, buf); };
```

| You have | Read it with | Write it with | Path |
|---|---|---|---|
| Your own struct | `req.body<User>()` | `res.json(user)` | Typed (Glaze, compile-time) |
| Arbitrary / ad-hoc JSON | `req.json()` | `res.json(Json{...})` | Dynamic (nlohmann DOM) |
| Struct + rules | `req.bind<Signup>(res)` | `res.json(value)` | Typed + [Validation](validation.md) |

The dynamic path is handy when the shape is unknown or you are assembling an ad-hoc object:

```cpp
app.get("/ping", [](Request& req, Response& res) {
    res.json(Json{{"pong", true}, {"ts", 1234567890}});
});

app.post("/echo", [](Request& req, Response& res) {
    Json doc = req.json();              // nlohmann DOM
    res.json(doc);                      // reflect it back
});
```

> `Json` is an alias for `nlohmann::json` (`include/json.hpp`). Reach for it when you genuinely need a runtime DOM; prefer typed structs when the shape is known, since they skip the DOM entirely.

## Output shape

For the `User` struct above, `res.json(u)` produces fields in declaration order, with names matching the members:

```json
{
  "id": 42,
  "name": "Ada",
  "active": true
}
```

`Content-Type` is set to `application/json` automatically by the typed overload.

## Common pitfalls

- **Not checking the parsed value.** `req.body<T>()` returns a default-constructed `T` on failure and never throws. Distinguishing "valid zeros" from "failed parse" is impossible from the return value, so validate or hand-check every required field. See [Validation](validation.md).
- **Expecting an exception or error code.** There is none surfaced to you — the internal `glz::read_json` error code is swallowed and converted to a default value.
- **Passing a string to `res.json(...)`.** A string-like argument is excluded by `GlazeSerializable` and will not hit the typed overload; use `res.text(...)` for plain text instead.
- **Passing a `Json` to the typed overload.** `nlohmann::json` is excluded from `GlazeSerializable`, so `res.json(Json{...})` correctly resolves to the dynamic overload, not the templated one.
- **Non-aggregate or private fields.** Glaze reflects plain aggregates; keep the struct simple (public data members, default-initialized) so reflection sees every field.

## See also

- [Requests and Responses](requests-and-responses.md) — the full `Request` / `Response` surface, including `body()`, `header()`, `status()`, and the convenience helpers.
- [Validation](validation.md) — declare field constraints once and turn a best-effort parse into a checked one with `validate<T>()` and `bind<T>(res)`.
