# Request validation

`req.body<T>()` parses JSON into a struct — it guarantees *shape and type*, nothing more. **Schema validation** adds *constraints*: required fields, numeric ranges, string lengths, regex patterns, and enums (the Ajv / Pydantic equivalent). It's **opt-in** and **declarative** — you describe the rules once, next to the type, and SwiftNet enforces them.

## Quick start

```cpp
#include "schema.hpp"
#include <optional>
#include <string>
using namespace swiftnet;

struct Signup {
    std::string name;
    int age{};
    std::string email;
    std::string role;
    std::optional<std::string> nickname;        // an optional field
};

// Declare the rules once, keyed by member pointer — field names are derived at compile time.
template <> struct swiftnet::schema<Signup> {
    static constexpr auto rules = swiftnet::rules(
        field<&Signup::name>(len(1, 50)),
        field<&Signup::age>(range(0, 150)),
        field<&Signup::email>(pattern("^[^@]+@[^@]+$")),
        field<&Signup::role>(one_of("admin", "user", "guest")),
        field<&Signup::nickname>(required{}, max_len(20)));
};

app.post("/signup", [](Request& req, Response& res) {
    auto s = req.bind<Signup>(res);   // parse + validate; on failure writes 400 JSON, returns nullopt
    if (!s) return;                   // stop here — the 400 has already been sent
    res.status(201).json(*s);         // *s is a validated Signup
});
```

## How it works

- **Opt-in.** A type with no `schema<T>` specialization validates as OK — `validate`/`bind` just parse it. Routes that never call them stay on the unchanged hot path.
- **Field names for free.** Constraints are keyed by member pointer (`field<&Signup::age>`); SwiftNet derives the JSON key at compile time (Glaze reflection), so you never repeat names as strings.
- **Compile-time type safety.** A string rule on a numeric field (or vice-versa) is a **compile error**, not a runtime surprise.

## Constraints

| Rule | Applies to | Error `rule` | Example |
|---|---|---|---|
| `required{}` | `std::optional<T>` only | `required` | `field<&S::nick>(required{})` |
| `min(v)` / `max(v)` | numeric | `min` / `max` | `field<&S::age>(min(0))` |
| `range(lo, hi)` | numeric | `range` | `field<&S::age>(range(0, 150))` |
| `min_len(n)` / `max_len(n)` | string | `min_length` / `max_length` | `field<&S::bio>(max_len(280))` |
| `len(lo, hi)` | string | `length` | `field<&S::name>(len(1, 50))` |
| `pattern("regex")` | string | `pattern` | `field<&S::email>(pattern("^[^@]+@[^@]+$"))` |
| `one_of(a, b, …)` | string or numeric | `one_of` | `field<&S::role>(one_of("admin","user"))` |

Combine several on one field: `field<&Signup::nickname>(required{}, max_len(20))`.

> Regex patterns are compiled once and cached per worker thread (`std::regex` isn't safe to construct concurrently), so repeated validation never recompiles them.

## The `required` + `std::optional` rule (read this)

> ⚠️ **`required{}` only does anything on `std::optional<T>` fields.** SwiftNet detects "missing" by whether the optional is engaged after parsing. A non-optional member always exists after parsing (default-constructed if the key was absent), so "absent" and "default value" are indistinguishable without re-parsing — which SwiftNet deliberately avoids. On a non-optional field, `required{}` is a **silent no-op**.
>
> **To make a field mandatory: declare it `std::optional<T>` and add `required{}`.**

```cpp
struct Account {
    std::optional<std::string> token;   // ✅ required works here
    std::string name;                   // ❌ required would be a no-op here
};
template <> struct swiftnet::schema<Account> {
    static constexpr auto rules = swiftnet::rules(
        field<&Account::token>(required{}));   // fires if "token" is absent
};
```

## `validate()` vs `bind()`

Two entry points on `Request`:

- **`req.bind<T>(res)` → `std::optional<T>`** — the one-liner. Parses, validates, and on failure **writes a 400 JSON response for you**, returning `std::nullopt`. On success, returns the value.
  ```cpp
  auto s = req.bind<Signup>(res);
  if (!s) return;            // 400 already sent
  use(*s);
  ```
- **`req.validate<T>()` → `Validated<T>`** — full control. Returns `{ bool ok; T value; std::vector<FieldError> errors; }`; you decide what to do with the errors. (`bind` is just `validate` + the default 400.)
  ```cpp
  auto v = req.validate<Signup>();
  if (!v.ok) { /* custom handling of v.errors */ }
  else       { use(v.value); }
  ```

`FieldError` is `{ std::string field; std::string rule; std::string message; }`.

## The 400 response

When `bind` fails it sends **HTTP 400** with `Content-Type: application/json` and *every* violation collected (not just the first):

```json
{
  "error": "validation_failed",
  "details": [
    { "field": "age",   "rule": "range",   "message": "age must be in [0, 150]" },
    { "field": "email", "rule": "pattern", "message": "email does not match required pattern" }
  ]
}
```

## Common pitfalls

- **"My `required` isn't firing."** The field isn't `std::optional<T>` — see the rule above.
- **Wrong-type constraint won't compile.** Intended: `len(...)` on an `int` is a compile error.
- **All errors are collected**, not just the first — read the whole `details` array / `v.errors`.

## See also

- [Typed JSON](typed-json.md) — the `req.body<T>()` / `res.json(T)` layer validation builds on
- [Plugins](plugins.md) — group validated routes behind a prefix with scoped middleware
