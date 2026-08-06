# AIPC Rust/C++ Media Service

AIPC is an RV1106 media service built around two processes:

- `aipc-daemon`: Rust/Tokio/Axum control plane, worker supervisor, REST/SSE API,
  H264 WebSocket preview bridge, persistent configuration and Vue static server.
- `media_worker`: C++17/RKMPI hardware worker for ISP, VI, VPSS, VENC, AI and
  AENC. It emits JSONL lifecycle events and publishes Annex-B H264 over inherited
  IPC.

The browser dashboard is implemented in Vue 3 under `webui/`.

## Host development

```bash
cargo test --workspace
npm --prefix webui test
npm --prefix webui run build

cmake --preset HostDebug -S media_worker
cmake --build media_worker/build/HostDebug
ctest --test-dir media_worker/build/HostDebug --output-on-failure
```

Host Cargo builds skip the hardware worker. The C++ host preset builds only
hardware-independent tests.

## RV1106 build and package

The Luckfox SDK defaults to the repository parent directory and can be
overridden with `AIPC_SDK_ROOT`.

```bash
./scripts/build-rv1106.sh
./scripts/package-rv1106.sh
```

The package is assembled at `target/package/aipc-rust` with `bin/`, `config/`,
`scripts/` and `www/` directories.

## Deploy and validate

```bash
AIPC_SKIP_BUILD=1 ./scripts/deploy-rv1106-adb.sh
./scripts/validate-rv1106-adb.sh
```

The default deployment directory is `/root/aipc-rust`. The daemon listens on
`0.0.0.0:8080` without authentication and must only be exposed to a trusted LAN.

## Repository layout

- `aipc-daemon/`: Rust daemon and Cargo-driven CMake integration.
- `media_worker/`: standalone C++17 RV1106 media process.
- `webui/`: Vue 3 management and live-preview UI.
- `config/`: packaged daemon configuration.
- `deploy/` and `scripts/`: board startup, build, package and validation flows.
- `3rdparty/luckfox_pico_rkmpi_example`: RKMPI headers and uClibc libraries.
- `3rdparty/nlohmann_json`: worker JSON dependency.
