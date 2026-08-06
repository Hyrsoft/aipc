use crate::config::PreviewConfig;
use crate::model::ServerEvent;
use axum::extract::ws::{Message, WebSocket};
use bytes::{Bytes, BytesMut};
use serde::Serialize;
use serde_json::json;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, RwLock};
use tokio::io::{AsyncRead, AsyncReadExt};
use tokio::sync::broadcast;

const HEADER_SIZE: usize = 28;
const MAGIC: &[u8; 4] = b"AIPV";
const VERSION: u16 = 1;
const KEYFRAME_FLAG: u16 = 1;

#[derive(Debug, Clone, Serialize)]
pub struct StreamInfo {
    pub generation: String,
    pub codec: &'static str,
    pub format: &'static str,
    pub width: i32,
    pub height: i32,
    pub fps: i32,
}

#[derive(Debug, Clone, Serialize)]
pub struct PreviewStatus {
    pub enabled: bool,
    pub available: bool,
    pub generation: Option<String>,
    pub clients: usize,
    pub max_clients: usize,
    pub frames_received: u64,
    pub bytes_received: u64,
    pub last_pts: Option<u64>,
    pub last_sequence: Option<u64>,
    pub malformed_frames: u64,
    pub lagged_clients: u64,
    pub stream: Option<StreamInfo>,
    pub last_error: Option<String>,
}

#[derive(Clone)]
pub struct PreviewFrame {
    pub info: StreamInfo,
    pub pts: u64,
    pub sequence: u64,
    pub keyframe: bool,
    pub data: Bytes,
}

#[derive(Debug, Clone)]
pub struct CodecConfig {
    pub info: StreamInfo,
    pub sps: Bytes,
    pub pps: Bytes,
}

#[derive(Clone)]
enum PreviewEvent {
    Frame(Arc<PreviewFrame>),
    State,
}

struct PreviewState {
    status: PreviewStatus,
    sps: Option<Bytes>,
    pps: Option<Bytes>,
    bootstrap: Option<Arc<PreviewFrame>>,
}

#[derive(Clone)]
pub struct PreviewHub {
    config: PreviewConfig,
    state: Arc<RwLock<PreviewState>>,
    sender: broadcast::Sender<PreviewEvent>,
    frames: broadcast::Sender<Arc<PreviewFrame>>,
    clients: Arc<AtomicUsize>,
    server_events: broadcast::Sender<ServerEvent>,
}

/// Public media-bus name used by non-preview consumers. The implementation is
/// kept in this module so the existing WebSocket preview path remains stable.
pub type VideoHub = PreviewHub;

pub struct PreviewClientGuard {
    clients: Arc<AtomicUsize>,
}

impl Drop for PreviewClientGuard {
    fn drop(&mut self) {
        self.clients.fetch_sub(1, Ordering::AcqRel);
    }
}

impl PreviewHub {
    pub fn new(config: PreviewConfig, server_events: broadcast::Sender<ServerEvent>) -> Self {
        let (sender, _) = broadcast::channel(config.broadcast_capacity.max(1));
        let (frames, _) = broadcast::channel(config.broadcast_capacity.max(64));
        let status = PreviewStatus {
            enabled: config.enabled,
            available: false,
            generation: None,
            clients: 0,
            max_clients: config.max_clients,
            frames_received: 0,
            bytes_received: 0,
            last_pts: None,
            last_sequence: None,
            malformed_frames: 0,
            lagged_clients: 0,
            stream: None,
            last_error: None,
        };
        Self {
            config,
            state: Arc::new(RwLock::new(PreviewState {
                status,
                sps: None,
                pps: None,
                bootstrap: None,
            })),
            sender,
            frames,
            clients: Arc::new(AtomicUsize::new(0)),
            server_events,
        }
    }

    pub fn enabled(&self) -> bool {
        self.config.enabled
    }

    pub fn max_frame_bytes(&self) -> usize {
        self.config.max_frame_bytes
    }

    pub fn status(&self) -> PreviewStatus {
        let mut status = self.state.read().unwrap().status.clone();
        status.clients = self.clients.load(Ordering::Acquire);
        status
    }

    pub fn subscribe(&self) -> broadcast::Receiver<Arc<PreviewFrame>> {
        self.frames.subscribe()
    }

    pub fn codec_config(&self) -> Option<CodecConfig> {
        let state = self.state.read().unwrap();
        let info = state.status.stream.clone()?;
        Some(CodecConfig {
            info,
            sps: strip_start_code(state.sps.as_ref()?),
            pps: strip_start_code(state.pps.as_ref()?),
        })
    }

    pub fn begin_generation(&self, info: StreamInfo) {
        let mut state = self.state.write().unwrap();
        state.status.available = false;
        state.status.generation = Some(info.generation.clone());
        state.status.stream = Some(info);
        state.status.frames_received = 0;
        state.status.bytes_received = 0;
        state.status.last_pts = None;
        state.status.last_sequence = None;
        state.status.malformed_frames = 0;
        state.status.last_error = None;
        state.sps = None;
        state.pps = None;
        state.bootstrap = None;
        drop(state);
        let _ = self.sender.send(PreviewEvent::State);
    }

    pub fn stop_generation(&self, generation: &str) {
        let mut state = self.state.write().unwrap();
        if state.status.generation.as_deref() != Some(generation) {
            return;
        }
        state.status.available = false;
        state.status.stream = None;
        state.bootstrap = None;
        drop(state);
        let _ = self.sender.send(PreviewEvent::State);
    }

    pub fn ingest(&self, frame: PreviewFrame) {
        let mut state = self.state.write().unwrap();
        if state.status.generation.as_deref() != Some(&frame.info.generation) {
            return;
        }
        for (kind, nal) in annex_b_nals(&frame.data) {
            if kind == 7 {
                state.sps = Some(nal);
            } else if kind == 8 {
                state.pps = Some(nal);
            }
        }
        state.status.frames_received += 1;
        state.status.bytes_received += frame.data.len() as u64;
        state.status.last_pts = Some(frame.pts);
        state.status.last_sequence = Some(frame.sequence);
        let frame = Arc::new(frame);
        if frame.keyframe {
            if let (Some(sps), Some(pps)) = (&state.sps, &state.pps) {
                let mut data = BytesMut::with_capacity(sps.len() + pps.len() + frame.data.len());
                data.extend_from_slice(sps);
                data.extend_from_slice(pps);
                data.extend_from_slice(&frame.data);
                state.bootstrap = Some(Arc::new(PreviewFrame {
                    data: data.freeze(),
                    ..(*frame).clone()
                }));
                state.status.available = true;
            }
        }
        drop(state);
        let _ = self.frames.send(frame.clone());
        let _ = self.sender.send(PreviewEvent::Frame(frame));
    }

    pub fn reader_error(&self, generation: &str, message: String) {
        let mut state = self.state.write().unwrap();
        if state.status.generation.as_deref() != Some(generation) {
            return;
        }
        state.status.available = false;
        state.status.malformed_frames += 1;
        state.status.last_error = Some(message.clone());
        drop(state);
        let _ = self.server_events.send(ServerEvent::new(
            "preview_warning",
            json!({"generation": generation, "message": message}),
        ));
        let _ = self.sender.send(PreviewEvent::State);
    }

    pub fn acquire_client(&self) -> Option<PreviewClientGuard> {
        self.clients
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                (current < self.config.max_clients).then_some(current + 1)
            })
            .ok()
            .map(|_| PreviewClientGuard {
                clients: self.clients.clone(),
            })
    }

    pub async fn serve_socket(&self, mut socket: WebSocket, _guard: PreviewClientGuard) {
        let mut receiver = self.sender.subscribe();
        let mut current_generation;
        if self.send_snapshot(&mut socket, true).await.is_err() {
            return;
        }
        current_generation = self.status().generation;
        loop {
            tokio::select! {
                incoming = socket.recv() => match incoming {
                    Some(Ok(Message::Close(_))) | None | Some(Err(_)) => break,
                    _ => {}
                },
                event = receiver.recv() => match event {
                    Ok(PreviewEvent::State) => {
                        if self.send_snapshot(&mut socket, true).await.is_err() { break; }
                        current_generation = self.status().generation;
                    }
                    Ok(PreviewEvent::Frame(frame)) => {
                        if current_generation.as_deref() != Some(frame.info.generation.as_str()) {
                            current_generation = Some(frame.info.generation.clone());
                            if send_json(&mut socket, json!({"type":"reset"})).await.is_err()
                                || send_json(&mut socket, json!({"type":"stream", "stream":frame.info})).await.is_err() {
                                break;
                            }
                            if let Some(bootstrap) = self.bootstrap() {
                                if socket.send(Message::Binary(bootstrap.data.clone())).await.is_err() { break; }
                            }
                            continue;
                        }
                        if socket.send(Message::Binary(frame.data.clone())).await.is_err() { break; }
                    }
                    Err(broadcast::error::RecvError::Lagged(skipped)) => {
                        self.mark_lagged(skipped);
                        if send_json(&mut socket, json!({"type":"reset", "reason":"lagged", "skipped":skipped})).await.is_err()
                            || self.send_snapshot(&mut socket, false).await.is_err() { break; }
                        current_generation = self.status().generation;
                    }
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }
        }
    }

    async fn send_snapshot(&self, socket: &mut WebSocket, reset: bool) -> Result<(), ()> {
        if reset {
            send_json(socket, json!({"type":"reset"})).await?;
        }
        let status = self.status();
        if let Some(stream) = status.stream {
            send_json(socket, json!({"type":"stream", "stream":stream})).await?;
            if let Some(bootstrap) = self.bootstrap() {
                socket
                    .send(Message::Binary(bootstrap.data.clone()))
                    .await
                    .map_err(|_| ())?;
            } else {
                send_json(socket, json!({"type":"state", "state":"waiting_keyframe"})).await?;
            }
        } else {
            send_json(socket, json!({"type":"state", "state":"stopped"})).await?;
        }
        Ok(())
    }

    fn bootstrap(&self) -> Option<Arc<PreviewFrame>> {
        self.state.read().unwrap().bootstrap.clone()
    }

    fn mark_lagged(&self, _skipped: u64) {
        self.state.write().unwrap().status.lagged_clients += 1;
    }
}

fn strip_start_code(nal: &Bytes) -> Bytes {
    if nal.starts_with(&[0, 0, 0, 1]) {
        nal.slice(4..)
    } else if nal.starts_with(&[0, 0, 1]) {
        nal.slice(3..)
    } else {
        nal.clone()
    }
}

async fn send_json(socket: &mut WebSocket, value: serde_json::Value) -> Result<(), ()> {
    socket
        .send(Message::Text(value.to_string().into()))
        .await
        .map_err(|_| ())
}

pub async fn read_video_ipc<R: AsyncRead + Unpin>(
    mut reader: R,
    hub: PreviewHub,
    info: StreamInfo,
) {
    let mut header = [0_u8; HEADER_SIZE];
    loop {
        match reader.read_exact(&mut header).await {
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(error) => {
                hub.reader_error(&info.generation, format!("read video IPC header: {error}"));
                break;
            }
        }
        if &header[0..4] != MAGIC || u16::from_be_bytes([header[4], header[5]]) != VERSION {
            hub.reader_error(&info.generation, "invalid video IPC header".into());
            break;
        }
        let flags = u16::from_be_bytes([header[6], header[7]]);
        let length = u32::from_be_bytes(header[8..12].try_into().unwrap()) as usize;
        if length == 0 || length > hub.max_frame_bytes() {
            hub.reader_error(
                &info.generation,
                format!("invalid video IPC payload length {length}"),
            );
            break;
        }
        let pts = u64::from_be_bytes(header[12..20].try_into().unwrap());
        let sequence = u64::from_be_bytes(header[20..28].try_into().unwrap());
        let mut payload = vec![0_u8; length];
        if let Err(error) = reader.read_exact(&mut payload).await {
            hub.reader_error(&info.generation, format!("read video IPC payload: {error}"));
            break;
        }
        hub.ingest(PreviewFrame {
            info: info.clone(),
            pts,
            sequence,
            keyframe: flags & KEYFRAME_FLAG != 0,
            data: Bytes::from(payload),
        });
    }
    hub.stop_generation(&info.generation);
}

pub fn annex_b_nals(data: &Bytes) -> Vec<(u8, Bytes)> {
    let mut starts = Vec::new();
    let mut index = 0;
    while index + 3 <= data.len() {
        let length = if index + 4 <= data.len() && data[index..index + 4] == [0, 0, 0, 1] {
            4
        } else if data[index..index + 3] == [0, 0, 1] {
            3
        } else {
            index += 1;
            continue;
        };
        starts.push((index, length));
        index += length;
    }
    starts
        .iter()
        .enumerate()
        .filter_map(|(position, (start, prefix))| {
            let nal_start = start + prefix;
            if nal_start >= data.len() {
                return None;
            }
            let end = starts.get(position + 1).map_or(data.len(), |item| item.0);
            Some((data[nal_start] & 0x1f, data.slice(*start..end)))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncWriteExt, duplex};

    fn test_hub(max_frame_bytes: usize) -> PreviewHub {
        let (events, _) = broadcast::channel(8);
        PreviewHub::new(
            PreviewConfig {
                max_frame_bytes,
                ..PreviewConfig::default()
            },
            events,
        )
    }

    fn info() -> StreamInfo {
        StreamInfo {
            generation: "g1".into(),
            codec: "h264",
            format: "annexb",
            width: 1280,
            height: 720,
            fps: 25,
        }
    }

    fn message(payload: &[u8], keyframe: bool) -> Vec<u8> {
        let mut output = Vec::new();
        output.extend_from_slice(MAGIC);
        output.extend_from_slice(&VERSION.to_be_bytes());
        output.extend_from_slice(&(if keyframe { 1_u16 } else { 0 }).to_be_bytes());
        output.extend_from_slice(&(payload.len() as u32).to_be_bytes());
        output.extend_from_slice(&42_u64.to_be_bytes());
        output.extend_from_slice(&7_u64.to_be_bytes());
        output.extend_from_slice(payload);
        output
    }

    #[tokio::test]
    async fn reads_fragmented_bootstrap_frame() {
        let hub = test_hub(4096);
        let info = info();
        hub.begin_generation(info.clone());
        let payload = [
            0, 0, 0, 1, 0x67, 0x64, 0, 0x28, 0, 0, 0, 1, 0x68, 1, 2, 3, 0, 0, 0, 1, 0x65, 9, 8, 7,
        ];
        let data = message(&payload, true);
        let (mut writer, reader) = duplex(8);
        let task = tokio::spawn(read_video_ipc(reader, hub.clone(), info));
        for chunk in data.chunks(3) {
            writer.write_all(chunk).await.unwrap();
        }
        tokio::time::sleep(std::time::Duration::from_millis(10)).await;
        assert!(hub.status().available);
        assert_eq!(hub.status().frames_received, 1);
        drop(writer);
        task.await.unwrap();
    }

    #[tokio::test]
    async fn rejects_oversized_frame() {
        let hub = test_hub(4);
        let info = info();
        hub.begin_generation(info.clone());
        let (mut writer, reader) = duplex(64);
        let task = tokio::spawn(read_video_ipc(reader, hub.clone(), info));
        writer
            .write_all(&message(&[1, 2, 3, 4, 5], false))
            .await
            .unwrap();
        drop(writer);
        task.await.unwrap();
        assert_eq!(hub.status().malformed_frames, 1);
    }

    #[tokio::test]
    async fn rejects_invalid_magic() {
        let hub = test_hub(4096);
        let info = info();
        hub.begin_generation(info.clone());
        let (mut writer, reader) = duplex(64);
        let task = tokio::spawn(read_video_ipc(reader, hub.clone(), info));
        let mut data = message(&[1, 2, 3], false);
        data[0] = b'X';
        writer.write_all(&data).await.unwrap();
        drop(writer);
        task.await.unwrap();
        assert_eq!(hub.status().malformed_frames, 1);
        assert!(
            hub.status()
                .last_error
                .as_deref()
                .unwrap()
                .contains("header")
        );
    }

    #[tokio::test]
    async fn slow_subscriber_lags_without_blocking_ingest() {
        let (events, _) = broadcast::channel(8);
        let hub = PreviewHub::new(
            PreviewConfig {
                broadcast_capacity: 1,
                ..PreviewConfig::default()
            },
            events,
        );
        let info = info();
        hub.begin_generation(info.clone());
        let mut receiver = hub.sender.subscribe();
        for sequence in 1..=3 {
            hub.ingest(PreviewFrame {
                info: info.clone(),
                pts: sequence,
                sequence,
                keyframe: false,
                data: Bytes::from_static(&[0, 0, 0, 1, 0x61]),
            });
        }
        assert!(matches!(
            receiver.recv().await,
            Err(broadcast::error::RecvError::Lagged(_))
        ));
        assert_eq!(hub.status().frames_received, 3);
    }

    #[test]
    fn enforces_client_limit() {
        let (events, _) = broadcast::channel(8);
        let hub = PreviewHub::new(
            PreviewConfig {
                max_clients: 1,
                ..PreviewConfig::default()
            },
            events,
        );
        let first = hub.acquire_client().unwrap();
        assert!(hub.acquire_client().is_none());
        drop(first);
        assert!(hub.acquire_client().is_some());
    }
}
