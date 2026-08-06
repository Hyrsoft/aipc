# Media Worker

Independent C++17 worker for the Luckfox Pico/RV1106 media hardware. It owns the
ISP, VI, VPSS, VENC, AI and AENC resources and publishes encoded H264 through
the inherited daemon IPC descriptor. Optional H264/G711A elementary-stream
debug dumps can be enabled through explicit paths. Lifecycle and metrics events are emitted
as one JSON object per stdout line; diagnostics from the vendor SDK remain on
stderr.

## Build

```bash
cmake --preset HostDebug
cmake --build --preset HostDebug
ctest --preset HostDebug

cmake --preset Release
cmake --build --preset Release
cmake --install build/Release
```

The cross toolchain derives the SDK root from the repository layout. Override it
with `LUCKFOX_SDK_ROOT=/path/to/luckfox-pico` when needed.

## Run

```bash
media_worker --config media_worker.example.json --duration-sec 10
```

Configuration values are loaded from defaults, then JSON, then explicit CLI
overrides. Run `media_worker --help` for the supported overrides.

Stop any board service that owns VI/VPSS/VENC/AI/AENC resources before starting
the worker. The installed `run_on_board.sh` stops the default `rkipc` service.

## ADB verification

```bash
./scripts/deploy_and_verify.sh
```

The script builds and installs the Release preset, deploys it to
`/root/media_worker`, runs a bounded capture, pulls the elementary streams and
uses host `ffprobe` to validate both outputs. It then performs three additional
start/SIGTERM/stop rounds to verify that all hardware channels are reusable.
