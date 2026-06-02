#ifndef swiftnet_detail_router_hpp
#define swiftnet_detail_router_hpp

// Compiled radix/trie router: replaces per-request std::regex matching (a top
// CPU cost in Gate A profiling) with an O(path-depth) tree walk. Supports static
// segments, ":param" captures, and a trailing "*" wildcard. Precedence at each
// node is static > param > wildcard, with backtracking so e.g. "/user/me" and
// "/user/:id" coexist correctly.
//
// Routes map to an opaque size_t index (into the owner's handler vector), keeping
// this header free of any swiftnet.hpp dependency. match() captures params as
// (name_view -> value_view): name views point into the tree (stable for the
// router's lifetime); value views point into the caller's `path` argument.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace swiftnet::detail
{

    class Router
    {
    public:
        static constexpr std::size_t npos = static_cast<std::size_t>(-1);
        using Params = std::vector<std::pair<std::string_view, std::string_view>>;

        void add(std::string_view method, std::string_view pattern, std::size_t index)
        {
            Node *n = &root_;
            for (std::string_view seg : segments(pattern))
            {
                if (!seg.empty() && seg.front() == ':')
                {
                    if (!n->param_child)
                    {
                        n->param_child = std::make_unique<Node>();
                        n->param_name = std::string(seg.substr(1));
                    }
                    n = n->param_child.get();
                }
                else if (seg == "*")
                {
                    if (!n->wildcard_child)
                        n->wildcard_child = std::make_unique<Node>();
                    n = n->wildcard_child.get();
                }
                else
                {
                    auto &child = n->static_children[std::string(seg)];
                    if (!child)
                        child = std::make_unique<Node>();
                    n = child.get();
                }
            }
            n->handlers.emplace(std::string(method), index);
        }

        std::size_t match(std::string_view method, std::string_view path, Params &params) const
        {
            // Split path into a stack buffer (no allocation for typical depths).
            std::string_view segs[kMaxSegments];
            std::size_t nseg = 0;
            for (std::string_view seg : segments(path))
            {
                if (nseg == kMaxSegments)
                    return npos; // pathologically deep path
                segs[nseg++] = seg;
            }
            return walk(&root_, segs, nseg, 0, method, params);
        }

    private:
        struct sv_hash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view s) const noexcept
            {
                return std::hash<std::string_view>{}(s);
            }
        };

        struct Node
        {
            std::unordered_map<std::string, std::unique_ptr<Node>, sv_hash, std::equal_to<>> static_children;
            std::unique_ptr<Node> param_child;
            std::string param_name;
            std::unique_ptr<Node> wildcard_child;
            std::unordered_map<std::string, std::size_t, sv_hash, std::equal_to<>> handlers;

            std::size_t handler_for(std::string_view method) const
            {
                auto it = handlers.find(method);
                return it == handlers.end() ? npos : it->second;
            }
        };

        static constexpr std::size_t kMaxSegments = 32;

        // Iterate "/a/b/c" -> a, b, c (skips empty segments / leading slash).
        struct seg_range
        {
            std::string_view s;
            struct iter
            {
                std::string_view s;
                std::size_t pos;
                std::string_view cur;
                void advance()
                {
                    while (pos < s.size() && s[pos] == '/')
                        ++pos;
                    if (pos >= s.size())
                    {
                        cur = {};
                        pos = std::string_view::npos;
                        return;
                    }
                    std::size_t start = pos;
                    while (pos < s.size() && s[pos] != '/')
                        ++pos;
                    cur = s.substr(start, pos - start);
                }
                std::string_view operator*() const { return cur; }
                iter &operator++()
                {
                    advance();
                    return *this;
                }
                bool operator!=(const iter &o) const { return pos != o.pos; }
            };
            iter begin() const
            {
                iter it{s, 0, {}};
                it.advance();
                return it;
            }
            iter end() const { return iter{s, std::string_view::npos, {}}; }
        };
        static seg_range segments(std::string_view s) { return seg_range{s}; }

        static std::size_t walk(const Node *n, const std::string_view *segs, std::size_t nseg,
                                std::size_t i, std::string_view method, Params &params)
        {
            if (i == nseg)
                return n->handler_for(method); // terminal

            std::string_view seg = segs[i];

            // 1. static
            if (auto it = n->static_children.find(seg); it != n->static_children.end())
            {
                std::size_t r = walk(it->second.get(), segs, nseg, i + 1, method, params);
                if (r != npos)
                    return r;
            }
            // 2. param
            if (n->param_child)
            {
                params.emplace_back(std::string_view(n->param_name), seg);
                std::size_t r = walk(n->param_child.get(), segs, nseg, i + 1, method, params);
                if (r != npos)
                    return r;
                params.pop_back();
            }
            // 3. wildcard (matches the rest)
            if (n->wildcard_child)
            {
                std::size_t r = n->wildcard_child->handler_for(method);
                if (r != npos)
                    return r;
            }
            return npos;
        }

        Node root_;
    };

} // namespace swiftnet::detail

#endif
