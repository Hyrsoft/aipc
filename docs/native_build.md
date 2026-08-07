# Native C++ build and dependency model

`native/` is the only top-level CMake entry point for the C++ part of AIPC. It
configures the shared protocol library, `media_worker`, and `ai_worker` in one
build tree. Cargo builds only `aipc-daemon`; `scripts/build-rv1106.sh` explicitly
builds native first and copies both installed workers next to the Rust binary
for package compatibility.

Pinned FetchContent inputs:

- nlohmann/json v3.12.0, SHA-256
  `4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187`
- Lua 5.4.8, SHA-256
  `4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae`
- VisionG v1.2.1, SHA-256
  `56336cc25150692e21505626b9f359b5dfeaa019f240460c2541b0bfdbe51bc0`
- Luckfox RKMPI commit `55178250c05542b156ac94c8c08cecef46589abf`,
  archive SHA-256
  `e7e7d761078f9a803de8d30a9f5dc836557f17278ebaf5583f3ad970caf853ce`

For a disconnected build, provide all required standard CMake overrides and
enable offline mode:

```bash
export FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/path/to/json
export FETCHCONTENT_SOURCE_DIR_LUA=/path/to/lua
export FETCHCONTENT_SOURCE_DIR_VISIONG=/path/to/visiong_cpp
export FETCHCONTENT_SOURCE_DIR_LUCKFOX_RKMPI=/path/to/luckfox_rkmpi
export AIPC_FETCHCONTENT_OFFLINE=ON
scripts/build-rv1106.sh
```

The configure step fails before compilation when an override is missing or a
required RKMPI library/header is absent. Online downloads are verified against
the hashes above. Runtime models remain separate and are fetched by
`scripts/fetch-ai-models.sh`.
