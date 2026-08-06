# Runtime architecture

The deployed service has one business entry point and one hardware worker.

## Rust daemon

`aipc-daemon` owns configuration, HTTP APIs, SSE events, WebSocket preview,
WebRTC distribution, MP4 recording, RTSP, static Web UI delivery and worker lifecycle decisions. Configuration changes use
a cold generation switch with validation and last-good rollback. Unexpected
worker exits use bounded restart backoff.

The Web form edits the Rust desired configuration. Rust validates it, writes a
generation-specific worker JSON, and starts C++ with that file. C++ only performs
defensive validation and execution; it does not own daemon runtime configuration.

The daemon creates two anonymous Unix socketpairs for each audio-enabled
generation: fd 3 carries AIPV/Annex-B H264 and fd 4 carries AIPA/G711A. Rust
distributes video to preview, MP4 recording, RTSP and WebRTC, and audio to
WebAudio preview, WAV recording and WebRTC PCMA. Slow consumers never block
hardware capture.

The WebRTC server uses a shared UDP listener and one `str0m` state machine per
browser session. It is ICE-lite and LAN-only in this release. H264 is sent
without transcoding and G711A is negotiated directly as PCMA. Sessions are
closed on worker generation changes and the browser falls back to the existing
WebSocket/MSE preview when WebRTC negotiation or connectivity fails.

Recordings atomically commit an MP4 and, when audio is available, a PCM16 WAV
companion aligned to the first video PTS. HTTP byte ranges are supported for both
media endpoints. RTSP remains video-only in this version.

## C++ media worker

`media_worker` exclusively owns ISP and RKMPI resources. The video path is ISP →
VI → VPSS → VENC/H264. The audio path is AI → AENC/G711A. Configuration is
injected as JSON with optional CLI overrides.

stdout is reserved for JSONL lifecycle and Metrics events. stderr contains SDK
diagnostics. Encoded media is published through bounded IPC writers. Optional
elementary-stream dumps are disabled by default and reserved for diagnostics;
they are not preview or recording sources.

## Build and deployment

Cargo invokes the worker CMake project for the RV1106 uClibc target. The package
script adds the Vue production bundle and board scripts. The board startup flow
stops the default `rkipc` service before starting the daemon so only the worker
owns media hardware.
