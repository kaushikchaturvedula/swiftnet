#pragma once
// Public entry point for SwiftNet schema-based request validation.
//
//   #include "schema.hpp"
//   struct User { std::string name; int age{}; std::optional<std::string> nick; };
//   template <> struct swiftnet::schema<User> {
//       static constexpr auto rules = swiftnet::rules(
//           swiftnet::field<&User::name>(required{}, len(1, 50)),
//           swiftnet::field<&User::age>(range(0, 150)),
//           swiftnet::field<&User::nick>(required{}, max_len(20)));   // optional member
//   };
//
// Then in a handler:
//   auto u = req.bind<User>(res);   // parse + validate; on failure writes 400 JSON, returns nullopt
//   if (!u) co_return;
//   // or: auto v = req.validate<User>();  -> Validated<User>{ ok, value, errors }
//
// A type with no schema<T> specialization validates as ok (degrades to req.body<T>()), so this
// layer is purely additive and the per-request hot path is untouched for routes that don't use it.
//
// Exposes (namespace swiftnet): schema<T>, has_schema_v<T>, FieldError, Validated<T>,
// ValidationErrorBody, the constraint verbs (required, min, max, range, min_len, max_len, len,
// pattern, one_of), and field<&T::m>(...) / rules(...). See detail/validate.hpp for the engine.

#include "detail/validate.hpp"
