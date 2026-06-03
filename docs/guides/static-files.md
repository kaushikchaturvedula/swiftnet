# Static files

Serve a directory of files (HTML, CSS, JS, images, downloads) straight from disk by mounting it at a URL prefix.

## Quick start

```cpp
#include "swiftnet.hpp"

using namespace swiftnet;

int main() {
    SwiftNet app;

    // Serve files in ./public under the /assets URL prefix.
    app.static_files("/assets", "./public");

    app.listen(8080, [] {
        // ./public/style.css  ->  GET /assets/style.css
        // ./public/img/logo.png -> GET /assets/img/logo.png
    });
}
```

`static_files(mount, root)` returns `SwiftNet&`, so it chains with your other route and middleware calls.

## How it works

- `static_files(mount, root)` registers a single `GET` route for `mount + "/*"` (a wildcard). It does not register any other verb.
- For each request, SwiftNet strips the `mount` prefix from the request path and appends the remainder to `root` to form the file path. So with `mount = "/assets"` and `root = "./public"`, a request for `/assets/img/logo.png` maps to `./public/img/logo.png`.
- If the resulting path contains `..` anywhere, the request is rejected with `403 Forbidden` (`"Directory traversal not allowed"`) before touching the filesystem.
- If the file exists and is a regular file, it is read fully into memory and sent with a `Content-Type` derived from its extension and a matching `Content-Length`. If it does not exist, the response is `404 Not Found`.
- The file is read whole into the response body on every request. There is no caching, no streaming, no `Range`/partial-content support, and no directory index (e.g. no automatic `index.html`).

> Files are loaded entirely into memory per request. This is fine for typical web assets; it is not suited to very large files or high-rate downloads.

## MIME types

The `Content-Type` is chosen from the file's lowercased extension. Unknown or extensionless files fall back to `application/octet-stream`.

| Extension(s) | `Content-Type` |
|---|---|
| `.html`, `.htm` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.json` | `application/json` |
| `.xml` | `application/xml` |
| `.txt` | `text/plain` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.svg` | `image/svg+xml` |
| `.ico` | `image/x-icon` |
| `.pdf` | `application/pdf` |
| `.zip` | `application/zip` |
| `.tar` | `application/x-tar` |
| `.gz` | `application/gzip` |
| `.mp3` | `audio/mpeg` |
| `.mp4` | `video/mp4` |
| `.avi` | `video/x-msvideo` |
| `.mov` | `video/quicktime` |
| anything else | `application/octet-stream` |

## Serving a single file

`static_files` is for a directory. To serve one specific file, use a normal route and `res.file(path)`, which applies the same MIME-type and `Content-Length` handling:

```cpp
app.get("/favicon.ico", [](Request& req, Response& res) {
    res.file("./public/favicon.ico"); // 404 automatically if missing
});
```

## Common pitfalls

- **No trailing slash on `mount`.** Use `"/assets"`, not `"/assets/"`. The route is built as `mount + "/*"`, so a trailing slash would create a double-slash pattern.
- **`root` is resolved relative to the process working directory.** A relative `root` like `"./public"` depends on where you launch the binary. Prefer an absolute path if your start directory varies.
- **No directory index.** Requesting the mount root or a subdirectory does not serve `index.html`; map such routes explicitly with `res.file(...)` if you need them.
- **Unknown extensions become `application/octet-stream`.** Browsers may download rather than render these. Add the correct extension to your files, or set `Content-Type` yourself on a custom route.

## See also

- [Routing](routing.md) — the wildcard and `:param` matching that `static_files` builds on.
- [Requests and responses](requests-and-responses.md) — `Response::file`, headers, and status helpers used here.
