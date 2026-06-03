#pragma once
// Schema-based request validation (Ajv-equivalent) for SwiftNet.
//
// Opt-in, declarative, compile-time-keyed. A developer specializes swiftnet::schema<T>
// with a constexpr `rules(...)` built from `field<&T::member>(constraints...)`. The field
// NAME is derived at compile time from the member pointer via Glaze (glz::get_name<P>()),
// so it is never repeated. validate<T>()/bind<T>() are the only entry points; routes that
// don't call them are unaffected (the schema<T> trait is compile-time; absent specialization
// => run_validation is a single `if constexpr` returning ok=true, i.e. it degrades to body<T>()).
//
// This is the implementation header; include "schema.hpp" (the public entry).

#include <glaze/glaze.hpp>

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace swiftnet
{
    // Specialize this with `static constexpr auto rules = swiftnet::rules(...)` to attach
    // validation to a (Glaze-aggregate) type. Unspecialized => "no schema" (validation is a no-op).
    template <class T>
    struct schema;

    // Does schema<T> carry rules?
    template <class T, class = void>
    struct has_schema : std::false_type {};
    template <class T>
    struct has_schema<T, std::void_t<decltype(schema<T>::rules)>> : std::true_type {};
    template <class T>
    inline constexpr bool has_schema_v = has_schema<T>::value;

    // One structured validation failure (plain aggregate => GlazeSerializable).
    struct FieldError
    {
        std::string field;   // e.g. "age"
        std::string rule;    // e.g. "max"
        std::string message; // e.g. "age must be <= 150"
    };

    // Result of validate<T>(): the parsed value plus any collected errors.
    template <class T>
    struct Validated
    {
        bool ok{false};
        T value{};
        std::vector<FieldError> errors;
        explicit operator bool() const noexcept { return ok; }
    };

    // The 400 response body shape (plain aggregate => GlazeSerializable).
    struct ValidationErrorBody
    {
        std::string error; // "validation_failed"
        std::vector<FieldError> details;
    };

    namespace detail
    {
        // ---- field-type categories + traits ----
        template <class F>
        concept Numeric = std::is_arithmetic_v<F> && !std::same_as<F, bool>;
        template <class F>
        concept StringLike = std::same_as<std::remove_cvref_t<F>, std::string>;

        template <class>
        struct is_optional : std::false_type {};
        template <class U>
        struct is_optional<std::optional<U>> : std::true_type {};
        template <class T>
        inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<T>>::value;

        // Marker base for the `required` constraint (detected via is_base_of).
        struct required_tag {};

        template <class V>
        std::string to_str(const V &v)
        {
            if constexpr (std::is_arithmetic_v<V>)
                return std::to_string(v);
            else
                return std::string(v);
        }

        // std::regex is expensive to compile and not safe to construct concurrently. Cache
        // one compiled regex per (engine thread, pattern-literal) so validated routes never
        // recompile per request and threads never share a std::regex. Patterns are constexpr
        // string literals, so pointer identity is a stable, sufficient cache key within a TU.
        inline const std::regex &cached_regex(const char *pat)
        {
            thread_local std::unordered_map<const char *, std::regex> cache;
            auto it = cache.find(pat);
            if (it == cache.end())
                it = cache.emplace(pat, std::regex(pat, std::regex::optimize)).first;
            return it->second;
        }

        // ---- constraint structs (each: static `rule` name + check(name,value)->optional<msg>) ----
        template <class V>
        struct min_c
        {
            V bound;
            static constexpr const char *rule = "min";
            template <class F>
            std::optional<std::string> check(std::string_view n, const F &v) const
            {
                static_assert(Numeric<F>, "min() applies only to numeric fields");
                if (!(v >= bound))
                    return std::string(n) + " must be >= " + to_str(bound);
                return std::nullopt;
            }
        };
        template <class V>
        struct max_c
        {
            V bound;
            static constexpr const char *rule = "max";
            template <class F>
            std::optional<std::string> check(std::string_view n, const F &v) const
            {
                static_assert(Numeric<F>, "max() applies only to numeric fields");
                if (!(v <= bound))
                    return std::string(n) + " must be <= " + to_str(bound);
                return std::nullopt;
            }
        };
        template <class V>
        struct range_c
        {
            V lo, hi;
            static constexpr const char *rule = "range";
            template <class F>
            std::optional<std::string> check(std::string_view n, const F &v) const
            {
                static_assert(Numeric<F>, "range() applies only to numeric fields");
                if (v < lo || v > hi)
                    return std::string(n) + " must be in [" + to_str(lo) + ", " + to_str(hi) + "]";
                return std::nullopt;
            }
        };
        template <class V>
        struct min_len_c
        {
            V bound;
            static constexpr const char *rule = "min_length";
            template <class F>
            std::optional<std::string> check(std::string_view n, const F &v) const
            {
                static_assert(StringLike<F>, "min_len() applies only to string fields");
                if (v.size() < static_cast<std::size_t>(bound))
                    return std::string(n) + " length must be >= " + to_str(bound);
                return std::nullopt;
            }
        };
        template <class V>
        struct max_len_c
        {
            V bound;
            static constexpr const char *rule = "max_length";
            template <class F>
            std::optional<std::string> check(std::string_view n, const F &v) const
            {
                static_assert(StringLike<F>, "max_len() applies only to string fields");
                if (v.size() > static_cast<std::size_t>(bound))
                    return std::string(n) + " length must be <= " + to_str(bound);
                return std::nullopt;
            }
        };
        template <class V>
        struct len_c
        {
            V lo, hi;
            static constexpr const char *rule = "length";
            template <class F>
            std::optional<std::string> check(std::string_view n, const F &v) const
            {
                static_assert(StringLike<F>, "len() applies only to string fields");
                if (v.size() < static_cast<std::size_t>(lo) || v.size() > static_cast<std::size_t>(hi))
                    return std::string(n) + " length must be in [" + to_str(lo) + ", " + to_str(hi) + "]";
                return std::nullopt;
            }
        };
        struct pattern_c
        {
            const char *pat;
            static constexpr const char *rule = "pattern";
            template <class F>
            std::optional<std::string> check(std::string_view n, const F &v) const
            {
                static_assert(StringLike<F>, "pattern() applies only to string fields");
                if (!std::regex_search(v, cached_regex(pat)))
                    return std::string(n) + " does not match required pattern";
                return std::nullopt;
            }
        };
        template <class... Vs>
        struct one_of_c
        {
            std::tuple<Vs...> allowed;
            static constexpr const char *rule = "one_of";
            template <class F>
            std::optional<std::string> check(std::string_view n, const F &v) const
            {
                const bool found = std::apply([&](const auto &...a) { return ((v == a) || ...); }, allowed);
                if (!found)
                    return std::string(n) + " is not an allowed value";
                return std::nullopt;
            }
        };

        // ---- the rule containers ----
        template <auto MemPtr, class... Cs>
        struct FieldRule
        {
            std::tuple<Cs...> constraints;
        };
        template <class... Fs>
        struct RuleSet
        {
            std::tuple<Fs...> fields;
        };

        // ---- driver: apply one constraint to a (possibly optional) member ----
        template <class M, class C>
        void apply_one(std::string_view name, const M &m, const C &c, std::vector<FieldError> &errs)
        {
            if constexpr (std::is_base_of_v<required_tag, C>)
            {
                if constexpr (is_optional_v<M>)
                {
                    if (!m.has_value())
                        errs.push_back({std::string(name), "required", std::string(name) + " is required"});
                }
                // non-optional members are always "present"; required is a no-op (documented).
            }
            else if constexpr (is_optional_v<M>)
            {
                if (m.has_value())
                    if (auto e = c.check(name, *m))
                        errs.push_back({std::string(name), C::rule, std::move(*e)});
                // absent optional: value constraints are skipped (only `required` can fire).
            }
            else
            {
                if (auto e = c.check(name, m))
                    errs.push_back({std::string(name), C::rule, std::move(*e)});
            }
        }

        template <class T, auto P, class... Cs>
        void apply_field(const T &obj, const FieldRule<P, Cs...> &fr, std::vector<FieldError> &errs)
        {
            constexpr std::string_view name = glz::get_name<P>(); // compile-time field name
            const auto &member = obj.*P;
            std::apply([&](const auto &...c) { (apply_one(name, member, c, errs), ...); }, fr.constraints);
        }

        // Run schema<T>::rules over `value`. No schema => ok. (Single if constexpr branch when absent.)
        template <class T>
        Validated<T> run_validation(T value)
        {
            Validated<T> out;
            if constexpr (has_schema_v<T>)
            {
                std::apply([&](const auto &...fr) { (apply_field(value, fr, out.errors), ...); },
                           schema<T>::rules.fields);
                out.ok = out.errors.empty();
            }
            else
            {
                out.ok = true;
            }
            out.value = std::move(value);
            return out;
        }

    } // namespace detail

    // ---- developer-facing constraint verbs (return detail constraint structs) ----
    // `required{}` is used as a literal; the others are value-constraint factories.
    struct required : detail::required_tag {};

    template <class V> constexpr auto min(V v) { return detail::min_c<V>{v}; }
    template <class V> constexpr auto max(V v) { return detail::max_c<V>{v}; }
    template <class V> constexpr auto range(V lo, V hi) { return detail::range_c<V>{lo, hi}; }
    template <class V> constexpr auto min_len(V n) { return detail::min_len_c<V>{n}; }
    template <class V> constexpr auto max_len(V n) { return detail::max_len_c<V>{n}; }
    template <class V> constexpr auto len(V lo, V hi) { return detail::len_c<V>{lo, hi}; }
    constexpr auto pattern(const char *p) { return detail::pattern_c{p}; }
    template <class... Vs> constexpr auto one_of(Vs... vs)
    {
        return detail::one_of_c<std::decay_t<Vs>...>{std::tuple<std::decay_t<Vs>...>{vs...}};
    }

    // `field<&T::member>(constraints...)` and `rules(fields...)`.
    template <auto MemPtr, class... Cs>
    constexpr auto field(Cs... cs)
    {
        return detail::FieldRule<MemPtr, Cs...>{std::tuple<Cs...>{cs...}};
    }
    template <class... Fs>
    constexpr auto rules(Fs... fs)
    {
        return detail::RuleSet<Fs...>{std::tuple<Fs...>{fs...}};
    }

} // namespace swiftnet
