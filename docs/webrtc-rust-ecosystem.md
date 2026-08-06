# Rust WebRTC ecosystem and AIPC selection

## Decision

AIPC uses `str0m = 0.21.0` with default features disabled and the `openssl`
backend enabled. The daemon provides a small signaling API and drives every
WebRTC state machine from a Tokio actor.

The first release is deliberately limited to trusted-LAN playback in modern
Chrome and Edge. It uses ICE-lite host candidates, H264 High Profile and PCMA.
There is no STUN, TURN, public signaling service, authentication or media
transcoding.

## Ecosystem comparison

| Option | Strengths | Costs for RV1106 | Result |
| --- | --- | --- | --- |
| `str0m 0.21` | Sans-I/O, complete ICE/DTLS/SRTP/RTP stack, H264 and PCMA packetizers, explicit buffering and timing | Application must own UDP polling, timeouts, signaling and session actors | Selected |
| `webrtc-rs 0.20` | Async API closer to `RTCPeerConnection`, broad Pion-derived protocol coverage | Larger runtime surface, less direct control over shared UDP and embedded scheduling; current 0.20 architecture is relatively new | Not selected for the board daemon |
| libwebrtc bindings | Best match with Chromium behavior and codec negotiation | Very large C++ toolchain/artifact, difficult ARMv7/uClibc integration and upgrades | Rejected |
| GStreamer WebRTC | Mature media graph and codec/plugin ecosystem | Requires adding GStreamer and plugin deployment to the rootfs; duplicates the existing RKMPI worker/media bus | Rejected |

`str0m` is not a media encoder. This is an advantage for AIPC: RKMPI already
produces browser-decodable H264 and G711A. `str0m` packetizes Annex-B H264 and
maps G711A directly to the standard PCMA RTP payload, so the Rust process does
not spend CPU transcoding to Opus.

## Crypto and cross-compilation

The apparent pure-Rust `str0m` crypto option still pulls `aws-lc-sys` through
the certificate-generation feature chain in version 0.21. That introduces an
unnecessary C build and cross-compilation risk on `armv7-unknown-linux-uclibceabihf`.

The Luckfox Buildroot already supplies OpenSSL 1.1.1 headers and shared
libraries. AIPC therefore uses the `str0m` OpenSSL backend and sets
`OPENSSL_DIR` to the target Buildroot sysroot during `scripts/build-rv1106.sh`.
The deployed daemon dynamically reuses `/usr/lib/libssl.so.1.1` and
`/usr/lib/libcrypto.so.1.1`; those libraries are already part of the board
rootfs. An isolated ARMv7/uClibc `cargo check` of this feature combination was
completed before integration.

## Runtime shape

- One UDP listener (default port 10000) is shared by all sessions.
- `Rtc::accepts` demultiplexes STUN, DTLS, SRTP and RTCP to the correct peer.
- Each peer is bound to one worker generation and is removed on generation
  changes, connection timeout, idle timeout or explicit DELETE.
- The actor follows str0m's mutation/drain invariant: one input or media write
  is followed by polling all output until the next timeout.
- A new or lagged peer waits for an IDR. Its first IDR is prefixed with the
  current SPS/PPS before H264 RTP packetization.
- Video and audio share a PTS anchor; video uses a 90 kHz RTP clock and PCMA
  uses an 8 kHz RTP clock.

## Signaling and compatibility

The browser creates recv-only transceivers and waits for local ICE gathering
before posting its SDP offer. The daemon returns a complete SDP answer and a
session ID. Trickle ICE is unnecessary for host-candidate LAN operation.

H264 `profile-level-id` is derived from the active SPS. The RV1106 worker is
currently configured for High Profile, so browsers that do not advertise a
compatible High Profile are rejected rather than receiving an undecodable
stream. Wider Safari/iOS support should be implemented later by making the
hardware encoder profile configurable, not by silently transcoding in Rust.

Linux Google Chrome 150 was observed to offer Baseline, Constrained Baseline,
Main and High 4:4:4 Predictive profiles, but not regular High (`64xxxx`). That
browser therefore takes the intentional WebSocket/MSE fallback with the
current worker stream. Chrome/Edge builds that advertise regular High can use
WebRTC directly; broad desktop compatibility ultimately requires a
browser-compatible hardware encoder profile.

Some RV1106 firmware emits the High Profile SPS constraint byte as `0x10`.
RFC 6184 and str0m classify interoperable High Profile SDP using a zero
profile-iop byte, so the signaling capability is normalized from `6410xx` to
`6400xx`; the original SPS and encoded stream are not rewritten. Both the raw
SPS value and normalized signaling value are exposed by the WebRTC status API.

For non-destructive board validation, the ignored
`webrtc::tests::live_board_transports_h264_and_pcma` test acts as a second
str0m peer and verifies signaling, ICE/DTLS, SRTP and receipt of both media
kinds. It creates and deletes one session and does not restart the worker or
write daemon configuration.

The configured MTU is treated as the maximum UDP WebRTC datagram size. str0m's
packetizer target is reduced by its documented 80-byte maximum RTP/SRTP header
and extension budget while the configured value remains the warning ceiling.
This avoids oversized H264 FU-A packets on the board LAN.
