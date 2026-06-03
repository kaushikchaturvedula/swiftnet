#pragma once
// Dual JSON facade:
//   * `Json` (= nlohmann::json) for dynamic/convenience JSON (parse arbitrary
//     documents, build ad-hoc objects). Used by Request::json() / Response::json(const Json&).
//   * Glaze for the TYPED fast path -- compile-time reflection, no runtime DOM:
//       struct User { int id; std::string name; };
//       res.json(user);            // glz::write_json, native speed
//       auto u = req.body<User>(); // glz::read_json into the struct
//
// The GlazeSerializable concept excludes nlohmann::json and string-like types so
// the templated Response::json<T> never competes with json(const Json&) or text().

#include <nlohmann/json.hpp>
#include <glaze/glaze.hpp>

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>

namespace swiftnet
{
    using Json = nlohmann::json;

    // A type usable with the typed (Glaze) JSON path: serializable by Glaze, but
    // NOT nlohmann::json (dynamic path) and NOT a string-like (those use text()).
    template <class T>
    concept GlazeSerializable =
        !std::same_as<std::remove_cvref_t<T>, Json> &&
        !std::convertible_to<const T &, std::string_view> &&
        requires(const T &v, std::string &buf) { glz::write_json(v, buf); };

} // namespace swiftnet
