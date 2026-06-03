#pragma once
// Fastify-style plugin encapsulation for SwiftNet.
//
//   app.plugin([](swiftnet::Scope& s){
//       s.use(authMiddleware);                       // scoped middleware (this scope + children)
//       s.decorate<std::shared_ptr<Db>>("db", db);   // scoped typed state (inherited; overridable)
//       auto db = s.get<std::shared_ptr<Db>>("db");  // read at registration, captured into handlers
//       s.get("/users", [db](Request&, Response& r){ ... });   // -> /v1/users
//   }, {.prefix = "/v1"});
//
// "Encapsulation" is a REGISTRATION-time concern with ZERO per-request overhead: a route's scoped
// middleware chain is resolved (root -> ... -> this) and composed into its handler_t at registration,
// then stored in the same global router/routes the rest of the app uses. The per-request hot path
// (handle_request_async / Router / Route) is unchanged; routes outside the scope are unaffected.
//
// Decorators are read-only after registration: get<T> returns shared_ptr<const T>, so a handler that
// captures a decorator cannot mutate shared state across engines (keeps the no-shared-mutable-state /
// TSan guarantee). Inherit: a child resolves own -> parent. Override: a child decorator shadows the
// parent's same key. Isolation: siblings have separate stores; lookup never walks siblings.

#include "swiftnet.hpp"
#include "detail/log.hpp"

#include <memory>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace swiftnet
{
    class Scope
    {
    public:
        // Route verbs mirror SwiftNet; return Scope& for chaining.
        template <class F> Scope &get(const std::string &p, F &&h)     { return add("GET", p, std::forward<F>(h)); }
        template <class F> Scope &post(const std::string &p, F &&h)    { return add("POST", p, std::forward<F>(h)); }
        template <class F> Scope &put(const std::string &p, F &&h)     { return add("PUT", p, std::forward<F>(h)); }
        template <class F> Scope &del(const std::string &p, F &&h)     { return add("DELETE", p, std::forward<F>(h)); }
        template <class F> Scope &patch(const std::string &p, F &&h)   { return add("PATCH", p, std::forward<F>(h)); }
        template <class F> Scope &options(const std::string &p, F &&h) { return add("OPTIONS", p, std::forward<F>(h)); }
        template <class F> Scope &head(const std::string &p, F &&h)    { return add("HEAD", p, std::forward<F>(h)); }

        // Scoped middleware: applies to routes registered on this scope and its descendants,
        // in root -> ... -> this order. Never applies to siblings or the parent's other routes.
        Scope &use(middleware_t mw)
        {
            mws_.push_back(std::move(mw));
            return *this;
        }

        // Nested plugin (a child scope). The child registers into the same app at composition time.
        template <class Fn>
        Scope &plugin(Fn &&fn, PluginOpts opts = {})
        {
            Scope child(app_, this, join_prefix(prefix_, opts.prefix));
            fn(child);
            return *this; // child dies here; its routes already captured everything by value
        }

        // Typed scoped state. shared_ptr keeps the decorated object alive past the (transient) Scope
        // once a handler captures it via get<T>().
        template <class T>
        Scope &decorate(const std::string &key, T value)
        {
            store_.insert_or_assign(key, Entry{std::type_index(typeid(T)), std::make_shared<T>(std::move(value))});
            return *this;
        }

        // Resolve own -> parent (child shadows parent); siblings are never consulted. Returns a
        // shared_ptr<const T> (read-only) -- or nullptr if the key is missing OR stored under a
        // different type (a safe miss, never UB). Debug builds log the miss to aid tracing.
        template <class T>
        std::shared_ptr<const T> get(const std::string &key) const
        {
            for (const Scope *s = this; s; s = s->parent_)
            {
                auto it = s->store_.find(key);
                if (it != s->store_.end())
                {
                    if (it->second.type == std::type_index(typeid(T)))
                        return std::static_pointer_cast<const T>(it->second.ptr);
                    SWIFTNET_LOG_DEBUG("Scope::get(\"{}\"): decorator exists but type mismatch (requested {}) -> nullptr",
                                       key, typeid(T).name());
                    return nullptr;
                }
            }
            SWIFTNET_LOG_DEBUG("Scope::get(\"{}\"): decorator not found -> nullptr", key);
            return nullptr;
        }

        const std::string &prefix() const noexcept { return prefix_; }

    private:
        friend class SwiftNet;

        Scope(SwiftNet &app, const Scope *parent, std::string prefix)
            : app_(app), parent_(parent), prefix_(std::move(prefix)) {}

        struct Entry
        {
            std::type_index type;
            std::shared_ptr<void> ptr;
        };

        // Resolve the effective middleware chain (root -> ... -> this) once, at registration.
        std::vector<middleware_t> effective_chain() const
        {
            std::vector<const Scope *> scopes; // this -> root
            for (const Scope *s = this; s; s = s->parent_)
                scopes.push_back(s);
            std::vector<middleware_t> out; // emit root -> this
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
                out.insert(out.end(), (*it)->mws_.begin(), (*it)->mws_.end());
            return out;
        }

        // Join a base path and a segment into a clean, single-slash, no-trailing-slash path.
        // base may be "" (root). seg may be "" (inherit base unchanged).
        static std::string join_prefix(const std::string &base, const std::string &seg)
        {
            if (seg.empty())
                return base;
            std::string s = base;
            if (!s.empty() && s.back() == '/')
                s.pop_back();
            if (seg.front() != '/')
                s += '/';
            s += seg;
            return s;
        }

        template <class F>
        Scope &add(const char *method, const std::string &path, F &&h)
        {
            std::string full = join_prefix(prefix_, path);
            handler_t user = SwiftNet::make_handler(std::forward<F>(h));
            std::vector<middleware_t> chain = effective_chain();
            if (chain.empty())
            {
                // No scoped middleware anywhere up the chain: register the handler directly so a
                // prefix-only plugin has byte-identical per-request cost to a hand-registered route.
                app_.add_route(method, full, std::move(user));
                return *this;
            }
            // Compose [scoped chain -> user handler] into one handler_t (captured BY VALUE, so it is
            // independent of this transient Scope). Reuses the shared curried-next() runner.
            handler_t composed =
                [chain = std::move(chain), user = std::move(user)](Request &req, Response &res) -> vthread
            {
                if (detail::run_chain(chain, req, res))
                    co_await user(req, res);
                co_return;
            };
            app_.add_route(method, full, std::move(composed));
            return *this;
        }

        SwiftNet &app_;
        const Scope *parent_; // raw ptr: parent strictly outlives child (stack-scoped nesting);
                              // only dereferenced during registration, never captured into handlers.
        std::string prefix_;
        std::vector<middleware_t> mws_;
        std::unordered_map<std::string, Entry> store_;
    };

    template <class Fn>
    SwiftNet &SwiftNet::plugin(Fn &&fn, PluginOpts opts)
    {
        Scope root(*this, nullptr, Scope::join_prefix(std::string(), opts.prefix));
        fn(root);
        return *this;
    }

} // namespace swiftnet
