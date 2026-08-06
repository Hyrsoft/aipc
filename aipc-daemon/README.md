# AIPC daemon

`aipc-daemon` is the Tokio/Axum control plane for the standalone RV1106
`media_worker`. It serializes lifecycle changes through a supervisor actor,
persists desired/active/pending/last-good configuration, and serves the Vue
dashboard and REST/SSE API.

Host development:

```sh
cargo test --workspace
npm --prefix webui run test
npm --prefix webui run build
```

RV1106 build and package:

```sh
scripts/build-rv1106.sh
scripts/package-rv1106.sh
scripts/deploy-rv1106-adb.sh
scripts/validate-rv1106-adb.sh
```

The packaged daemon listens on `0.0.0.0:8080` without authentication and must
only be exposed on a trusted LAN.

Live H264/G711A preview is available at `/api/v1/preview/ws`. The daemon receives
framed Annex-B video (`AIPV`, fd 3) and G711A audio (`AIPA`, fd 4) over separate
inherited Unix socketpairs. The browser uses jMuxer/MSE for video and a bounded
WebAudio jitter queue with an in-browser G711A decoder for audio.

Rust is the daemon runtime's only configuration source. `PUT /api/v1/config`
updates desired state, validates every audio hardware/codec field, writes a
generation-specific worker JSON, waits for enabled streams, and commits or
rolls back. C++ defaults only support standalone tests.

Recordings are committed as an MP4 video plus an optional PCM16 WAV companion.
The WAV is available at `/api/v1/recordings/{id}/audio`; list, delete and ZIP
export operations cover both files.

Worker elementary-stream outputs are disabled by default. They remain available
only as explicit diagnostic dumps. IPC audio is the formal source for preview
and recording; `audio.output_path` is never a business data channel.
