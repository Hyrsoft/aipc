# AI worker third-party components

The RV1106 deployment package contains the following pinned components:

- VisionG v1.2.1, `visiong_cpp.zip`
  - License: LGPL-3.0-or-later (including the notices shipped with the release)
  - SHA-256: `56336cc25150692e21505626b9f359b5dfeaa019f240460c2541b0bfdbe51bc0`
  - Source/release: <https://github.com/yiex/visiong/releases/tag/v1.2.1>
  - The package dynamically links `libvisiong.so`. Corresponding source can be
    obtained from the release above or the repository history for tag `v1.2.1`.
- Lua 5.4.8
  - License: MIT
  - SHA-256: `4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae`
  - Source: <https://www.lua.org/ftp/lua-5.4.8.tar.gz>
  - Lua is statically linked into `ai_worker`.
- YOLOv5n COCO80 RKNN example model
  - SHA-256: `083b2cf8983a9956cb203b3cce1bb83e26690cc9429c7e07d2fd337b06fcccec`
- COCO80 label list
  - SHA-256: `d7654b26101572841ed1cd80aa03aa60e35f1b8acb4aea6906c4066886f16e07`

CMake FetchContent verifies native dependency hashes before configuration. For
offline builds, set `FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON`,
`FETCHCONTENT_SOURCE_DIR_LUA`, `FETCHCONTENT_SOURCE_DIR_VISIONG`, and
`FETCHCONTENT_SOURCE_DIR_LUCKFOX_RKMPI`, then enable
`AIPC_FETCHCONTENT_OFFLINE`. `scripts/fetch-ai-models.sh` independently verifies
the runtime model and label hashes.
