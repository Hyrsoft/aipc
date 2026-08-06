# Runtime architecture

The deployed service has one business entry point and one hardware worker.

## Rust daemon

`aipc-daemon` owns configuration, HTTP APIs, SSE events, WebSocket preview,
static Web UI delivery and worker lifecycle decisions. Configuration changes use
a cold generation switch with validation and last-good rollback. Unexpected
worker exits use bounded restart backoff.

The daemon creates an anonymous Unix socketpair for each generation, passes the
child endpoint as file descriptor 3 and broadcasts received Annex-B H264 access
units to browser clients. Slow preview clients never block media capture.

## C++ media worker

`media_worker` exclusively owns ISP and RKMPI resources. The video path is ISP →
VI → VPSS → VENC/H264. The audio path is AI → AENC/G711A. Configuration is
injected as JSON with optional CLI overrides.

stdout is reserved for JSONL lifecycle and Metrics events. stderr contains SDK
diagnostics. Encoded video is published to the inherited IPC descriptor while
configured elementary-stream outputs remain independent.

## Build and deployment

Cargo invokes the worker CMake project for the RV1106 uClibc target. The package
script adds the Vue production bundle and board scripts. The board startup flow
stops the default `rkipc` service before starting the daemon so only the worker
owns media hardware.
