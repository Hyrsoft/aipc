# Media Worker

Independent C++17 worker for the Luckfox Pico/RV1106 media hardware. It owns the
ISP, VI, VPSS, VENC, AI and AENC resources and writes H264/G711A elementary
streams to externally configured files. Lifecycle and metrics events are emitted
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

The worker intentionally does not stop an existing `aipc` process. Stop all
processes that own the media hardware before starting it. The installed
`run_on_board.sh` stops the default `rkipc` service after checking this condition.

## ADB verification

```bash
./scripts/deploy_and_verify.sh
```

The script builds and installs the Release preset, deploys it to
`/root/media_worker`, runs a bounded capture, pulls the elementary streams and
uses host `ffprobe` to validate both outputs. It then performs three additional
start/SIGTERM/stop rounds to verify that all hardware channels are reusable.
