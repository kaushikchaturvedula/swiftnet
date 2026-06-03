# Async handlers & CPU offload

SwiftNet handlers can be plain synchronous functions, or coroutines that `co_await` async work without blocking the engine that owns the connection. For CPU-heavy work, `swiftnet::offload(...)` moves the computation onto a stealable compute task so the engine stays free to drive its other I/O-bound connections.

## Quick start

```cpp
#include "swiftnet.hpp"
#include "vthread_scheduler.hpp" // swiftnet::offload
#include <cstdint>

using namespace swiftnet;

int main()
{
    SwiftNet app(8080);

    // An async handler returns `vthread` instead of void. Inside it you may
    // co_await offload(...) to run CPU-heavy work off the connection's engine,
    // then resume on that same engine and respond.
    app.get("/work", [](Request &, Response &res) -> vthread {
        std::uint64_t acc = 0;
        co_await swiftnet::offload([&acc] {
            for (long i = 0; i < 2'000'000; ++i)
                acc += static_cast<std::uint64_t>(i) * 2654435761u;
        });
        Json j;
        j["acc"] = acc;   // the value computed inside the offloaded task
        res.json(j);
        co_return;
    });

    app.listen([] { /* listening on :8080 */ });
    return 0;
}
```

```bash
curl localhost:8080/work
# {"acc":<computed sum>}
```

The lambda captures `acc` by reference, fills it in on the compute task, and the value is serialized into the response after the `co_await` resumes. Capture everything the result depends on, and make sure each captured object outlives the `co_await` — here `acc` lives in the coroutine frame for the whole handler, so it is safe.

## How it works

- A handler is **either** a sync `void(Request&, Response&)` **or** an async coroutine `vthread(Request&, Response&)`. Both signatures are accepted by every route verb (`get`, `post`, `put`, `del`, `patch`, `options`, `head`).
- `vthread` is SwiftNet's coroutine task type (a lazily-started, move-only "virtual thread"). You opt into the async form simply by writing `-> vthread` and using `co_await` / `co_return` in the body.
- `swiftnet::offload(fn)` returns an awaitable. When you `co_await` it, the handler suspends, `fn` is enqueued as a **stealable compute task**, and the engine that owns the connection is released to service other sockets instead of spinning on your CPU work.
- When the compute task finishes, the handler **resumes on its original owning engine**, so the `Request`/`Response` and any captured state are touched from the same engine that has always owned them.
- The offloaded `fn` has signature `std::function<void()>` — it returns nothing. Communicate results by capturing a variable by reference (as above) and writing into it; read that variable after the `co_await` returns.
- If the compute backlog is short enough, the scheduler may run `fn` inline and resume immediately rather than suspending — an optimization that does not change the observable behavior of your handler.

> Note: `offload` lives in `vthread_scheduler.hpp`, which is separate from `swiftnet.hpp`. Include `"vthread_scheduler.hpp"` in any translation unit that calls `swiftnet::offload`.

## When to use offload (and when not to)

| Work in your handler | Use a coroutine? | Use `offload`? |
| --- | --- | --- |
| Plain async I/O (`co_await` on a socket, timer, downstream call) | Yes | No |
| Tight CPU loop: hashing, compression, parsing a large blob, image/number crunching | Yes | Yes |
| Trivial synchronous logic (build a small JSON, look up a value) | No (sync handler is fine) | No |

The rule of thumb: **`co_await` async I/O directly; reach for `offload` only when the work is CPU-bound and long enough that it would otherwise monopolize the engine** and stall the other connections that engine is responsible for. Offloading a few microseconds of work just adds scheduling overhead.

> ⚠️ Do not call blocking OS APIs (synchronous file reads, blocking `sleep`, blocking DB drivers) inside an `offload` lambda expecting it to "become async." Compute tasks run on worker capacity, not an unbounded thread pool; a blocking call there occupies that capacity. `offload` is for CPU work that runs to completion, not for parking on a blocking syscall.

## API variants

```cpp
// 1) Sync handler — no coroutine, no offload.
app.get("/sample", [](Request &, Response &res) {
    res.json(User{1, "ada", true});
});

// 2) Async handler that only does async I/O — coroutine, no offload.
app.get("/proxy", [](Request &req, Response &res) -> vthread {
    // co_await some async downstream call here ...
    res.ok();
    co_return;
});

// 3) Async handler with CPU offload — coroutine + offload.
app.get("/heavy", [](Request &, Response &res) -> vthread {
    std::uint64_t result = 0;
    co_await swiftnet::offload([&result] { result = /* CPU-heavy work */ 0; });
    Json j; j["result"] = result;
    res.json(j);
    co_return;
});
```

The offload signature is:

```cpp
namespace swiftnet {
    offload_awaitable offload(std::function<void()> fn);
}
```

`co_await offload(fn)` yields nothing (`await_resume()` returns `void`) — you read results from the variables `fn` captured.

## Common pitfalls

- **Forgetting `co_return`.** A handler returning `vthread` is a coroutine; end it with `co_return` (or fall off the end after your last `co_await`). Mixing a plain `return;` with `co_await` in the same function is a compile error.
- **Returning the result from the lambda.** `offload` takes a `void()`; a value you `return` from the lambda is discarded. Capture by reference and assign instead.
- **Dangling captures.** Anything captured by reference must outlive the `co_await`. Locals in the coroutine frame (like `acc`) are fine; references to temporaries are not.
- **Offloading tiny work.** For sub-microsecond work the scheduling cost outweighs the benefit — just compute it inline in a sync handler.
- **Calling `offload` from a sync handler.** `co_await` is only valid inside a coroutine. If you need offload, the handler must return `vthread`.
- **Expecting offload for blocking I/O.** It does not make a blocking syscall non-blocking; use real async I/O for I/O, and reserve offload for CPU.

## Performance

On the measured macOS/M1/kqueue backend, offloading CPU-bound work keeps per-request latency on I/O-bound connections from being dominated by an unrelated compute spike on the same engine. The stealable-task mechanics and the compute-bound scheduler experiment behind them are described separately — note that the work-stealing valve result is a compute-bound scheduler measurement, not a web-throughput claim. See [the benchmarks](../architecture/overview.md) for what is and isn't measured.

## See also

- [Requests & responses](requests-and-responses.md) — the `Request`/`Response` API used in every handler.
- [The work-stealing valve](../architecture/work-stealing-valve.md) — how stealable compute tasks are balanced across engines.
- [Architecture overview](../architecture/overview.md) — engines, connection ownership, and the I/O backends.
