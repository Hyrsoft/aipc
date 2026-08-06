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

Live H264 preview is available at `/api/v1/preview/ws`. The daemon receives
framed Annex-B access units from the worker over an inherited Unix socketpair;
the browser uses the locally bundled jMuxer/MSE player. Preview failure never
stops the media pipeline or Rust-managed recording.

Worker elementary-stream outputs are disabled by default. They remain available
only as explicit diagnostic dumps; normal MP4 recording is managed by the Rust
daemon and stored under its configured recording directory.
