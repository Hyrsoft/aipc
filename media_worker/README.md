# Media Worker

C++17 worker for the Luckfox Pico/RV1106 media hardware. It owns the
ISP, VI, VPSS, VENC, AI and AENC resources and publishes encoded H264 and G711A
through inherited daemon IPC descriptors. Optional H264/G711A elementary-stream
debug dumps can be enabled through explicit paths. Lifecycle and metrics events are emitted
as one JSON object per stdout line; diagnostics from the vendor SDK remain on
stderr.

## Build

```bash
cd ../native
cmake --preset HostDebug
cmake --build --preset HostDebug
ctest --preset HostDebug
```

`media_worker` is a target of the repository `native/` project. It no longer has
an independent preset, toolchain, dependency discovery path, or deployment
script. Use `scripts/build-rv1106.sh`, `scripts/package-rv1106.sh`, and
`scripts/deploy-rv1106-adb.sh` from the repository root for RV1106 deployment.

## Run

```bash
media_worker --config media_worker.example.json --duration-sec 10
```

Configuration values are loaded from defaults, then JSON, then explicit CLI
overrides. Run `media_worker --help` for the supported overrides.

Daemon launches pass `--video-ipc-fd 3 --audio-ipc-fd 4`. Video keeps AIPV v1
framing; audio uses AIPA v1 with a 28-byte big-endian header containing version,
flags, payload length, PTS and sequence. Audio publication uses a bounded writer
queue: overflow drops old audio without blocking AENC, while a socket write
failure emits `FatalError(media=audio)`.

The daemon-generated JSON is authoritative in managed operation. Worker-side
defaults exist only for standalone and test use. `output_path` values are
optional diagnostic dumps and should remain empty in normal deployments.

In managed operation the packaged daemon owns startup and shutdown of the media
worker and stops conflicting board services through the root deployment scripts.
