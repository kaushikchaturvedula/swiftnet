# rapidyaml (vendored, amalgamated single header)

`ryml_all.hpp` is the official amalgamated single-header build of
[rapidyaml](https://github.com/biojppm/rapidyaml) (includes its c4core dependency),
generated via `tools/amalgamate.py`. Vendored (rather than FetchContent) to avoid the
c4core submodule recursion and a separate library build, and for reproducible offline builds.

- Upstream: https://github.com/biojppm/rapidyaml
- Version: v0.7.2
- Regenerate: `git clone --recurse-submodules https://github.com/biojppm/rapidyaml && cd rapidyaml && python3 tools/amalgamate.py ryml_all.hpp`
- Usage: define `RYML_SINGLE_HDR_DEFINE_NOW` in exactly ONE translation unit before including.
