use crate::config::AiInputConfig;
use crate::model::{ServerEvent, now_ms};
use bytes::Bytes;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::sync::{Arc, RwLock};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::sync::{Mutex, broadcast, watch};
use uuid::Uuid;

pub const AIPF_HEADER_SIZE: usize = 88;
const AIPF_MAGIC: &[u8; 4] = b"AIPF";
const AIPF_VERSION: u16 = 1;
const MAX_CONTROL_BYTES: usize = 256 * 1024;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum AiFitMode {
    Stretch,
    Contain,
    Cover,
}

impl AiFitMode {
    fn from_wire(value: u16) -> Option<Self> {
        match value {
            0 => Some(Self::Stretch),
            1 => Some(Self::Contain),
            2 => Some(Self::Cover),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct AiFrameTransform {
    pub crop_x: i32,
    pub crop_y: i32,
    pub crop_width: i32,
    pub crop_height: i32,
    pub pad_left: i32,
    pub pad_top: i32,
    pub pad_right: i32,
    pub pad_bottom: i32,
}

#[derive(Debug, Clone)]
pub struct AiFrame {
    pub generation: String,
    pub pts: u64,
    pub sequence: u64,
    pub width: u32,
    pub height: u32,
    pub y_stride: u32,
    pub uv_stride: u32,
    pub height_stride: u32,
    pub main_width: u32,
    pub main_height: u32,
    pub fit_mode: AiFitMode,
    pub transform: AiFrameTransform,
    pub data: Bytes,
}

pub fn encode_ai_frame(frame: &AiFrame) -> Vec<u8> {
    let mut output = Vec::with_capacity(AIPF_HEADER_SIZE + frame.data.len());
    output.extend_from_slice(AIPF_MAGIC);
    output.extend_from_slice(&AIPF_VERSION.to_be_bytes());
    let fit = match frame.fit_mode {
        AiFitMode::Stretch => 0_u16,
        AiFitMode::Contain => 1,
        AiFitMode::Cover => 2,
    };
    output.extend_from_slice(&fit.to_be_bytes());
    output.extend_from_slice(&(frame.data.len() as u32).to_be_bytes());
    output.extend_from_slice(&frame.pts.to_be_bytes());
    output.extend_from_slice(&frame.sequence.to_be_bytes());
    for value in [
        frame.width,
        frame.height,
        frame.y_stride,
        frame.uv_stride,
        frame.height_stride,
        frame.main_width,
        frame.main_height,
    ] {
        output.extend_from_slice(&value.to_be_bytes());
    }
    for value in [
        frame.transform.crop_x,
        frame.transform.crop_y,
        frame.transform.crop_width,
        frame.transform.crop_height,
        frame.transform.pad_left,
        frame.transform.pad_top,
        frame.transform.pad_right,
        frame.transform.pad_bottom,
    ] {
        output.extend_from_slice(&value.to_be_bytes());
    }
    output.extend_from_slice(&frame.data);
    output
}

#[derive(Debug, Clone, Serialize)]
pub struct AiInputStatus {
    pub generation: Option<String>,
    pub available: bool,
    pub frames_received: u64,
    pub bytes_received: u64,
    pub malformed_frames: u64,
    pub last_sequence: Option<u64>,
    pub last_pts: Option<u64>,
    pub last_frame_at_ms: Option<u64>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub y_stride: Option<u32>,
    pub last_error: Option<String>,
    pub config: Option<AiInputConfig>,
    pub control_available: bool,
}

impl Default for AiInputStatus {
    fn default() -> Self {
        Self {
            generation: None,
            available: false,
            frames_received: 0,
            bytes_received: 0,
            malformed_frames: 0,
            last_sequence: None,
            last_pts: None,
            last_frame_at_ms: None,
            width: None,
            height: None,
            y_stride: None,
            last_error: None,
            config: None,
            control_available: false,
        }
    }
}

#[derive(Clone)]
pub struct MediaControlClient {
    generation: String,
    stream: Arc<Mutex<UnixStream>>,
}

impl MediaControlClient {
    pub fn new(generation: String, stream: UnixStream) -> Self {
        Self {
            generation,
            stream: Arc::new(Mutex::new(stream)),
        }
    }

    pub fn generation(&self) -> &str {
        &self.generation
    }

    pub async fn request(&self, command: &str, body: Value) -> anyhow::Result<Value> {
        let request_id = Uuid::new_v4().to_string();
        let mut request = json!({
            "version": 1,
            "request_id": request_id,
            "command": command,
        });
        if let (Some(target), Some(source)) = (request.as_object_mut(), body.as_object()) {
            target.extend(source.clone());
        }
        let payload = serde_json::to_vec(&request)?;
        anyhow::ensure!(
            payload.len() <= MAX_CONTROL_BYTES,
            "media control request is too large"
        );
        let mut stream = self.stream.lock().await;
        stream
            .write_all(&(payload.len() as u32).to_be_bytes())
            .await?;
        stream.write_all(&payload).await?;
        let length = stream.read_u32().await? as usize;
        anyhow::ensure!(
            (1..=MAX_CONTROL_BYTES).contains(&length),
            "invalid media control response length {length}"
        );
        let mut response = vec![0_u8; length];
        stream.read_exact(&mut response).await?;
        let response: Value = serde_json::from_slice(&response)?;
        anyhow::ensure!(
            response.get("request_id").and_then(Value::as_str) == Some(&request_id),
            "media control response request_id mismatch"
        );
        if response.get("type").and_then(Value::as_str) == Some("error") {
            anyhow::bail!(
                "{}",
                response
                    .get("error")
                    .map(Value::to_string)
                    .unwrap_or_else(|| "media control rejected request".into())
            );
        }
        Ok(response)
    }
}

#[derive(Clone)]
pub struct AiHub {
    frames: watch::Sender<Option<Arc<AiFrame>>>,
    status: Arc<RwLock<AiInputStatus>>,
    control: Arc<RwLock<Option<MediaControlClient>>>,
    max_frame_bytes: usize,
    events: broadcast::Sender<ServerEvent>,
}

impl AiHub {
    pub fn new(max_frame_bytes: usize, events: broadcast::Sender<ServerEvent>) -> Self {
        let (frames, _) = watch::channel(None);
        Self {
            frames,
            status: Arc::new(RwLock::new(AiInputStatus::default())),
            control: Arc::new(RwLock::new(None)),
            max_frame_bytes,
            events,
        }
    }

    pub fn status(&self) -> AiInputStatus {
        self.status.read().unwrap().clone()
    }

    pub fn subscribe_frames(&self) -> watch::Receiver<Option<Arc<AiFrame>>> {
        self.frames.subscribe()
    }

    pub fn begin_generation(&self, generation: String, config: AiInputConfig) {
        self.frames.send_replace(None);
        *self.status.write().unwrap() = AiInputStatus {
            generation: Some(generation),
            config: Some(config),
            ..AiInputStatus::default()
        };
    }

    pub fn set_control(&self, control: MediaControlClient) {
        let generation = control.generation().to_owned();
        *self.control.write().unwrap() = Some(control);
        let mut status = self.status.write().unwrap();
        if status.generation.as_deref() == Some(&generation) {
            status.control_available = true;
        }
    }

    pub fn clear_generation(&self, generation: &str) {
        self.frames.send_replace(None);
        if self.status.read().unwrap().generation.as_deref() == Some(generation) {
            self.status.write().unwrap().available = false;
        }
        if self
            .control
            .read()
            .unwrap()
            .as_ref()
            .is_some_and(|item| item.generation() == generation)
        {
            *self.control.write().unwrap() = None;
            self.status.write().unwrap().control_available = false;
        }
    }

    pub async fn configure_input(&self, config: AiInputConfig) -> anyhow::Result<Value> {
        let control = self
            .control
            .read()
            .unwrap()
            .clone()
            .ok_or_else(|| anyhow::anyhow!("media control is not available"))?;
        control.request("pause_ai_frames", json!({})).await?;
        let response = match control
            .request("configure_ai_channel", json!({"ai_input": config}))
            .await
        {
            Ok(response) => response,
            Err(error) => {
                let _ = control.request("resume_ai_frames", json!({})).await;
                return Err(error);
            }
        };
        if config.enabled {
            control.request("resume_ai_frames", json!({})).await?;
        }
        self.status.write().unwrap().config = Some(config);
        Ok(response)
    }

    pub async fn media_request(&self, command: &str, body: Value) -> anyhow::Result<Value> {
        let control = self
            .control
            .read()
            .unwrap()
            .clone()
            .ok_or_else(|| anyhow::anyhow!("media control is not available"))?;
        control.request(command, body).await
    }

    fn ingest(&self, frame: AiFrame) {
        let frame = Arc::new(frame);
        {
            let mut status = self.status.write().unwrap();
            if status.generation.as_deref() != Some(&frame.generation) {
                return;
            }
            status.available = true;
            status.frames_received += 1;
            status.bytes_received += frame.data.len() as u64;
            status.last_sequence = Some(frame.sequence);
            status.last_pts = Some(frame.pts);
            status.last_frame_at_ms = Some(now_ms());
            status.width = Some(frame.width);
            status.height = Some(frame.height);
            status.y_stride = Some(frame.y_stride);
            status.last_error = None;
        }
        self.frames.send_replace(Some(frame));
    }

    fn reader_error(&self, generation: &str, error: String) {
        let mut status = self.status.write().unwrap();
        if status.generation.as_deref() != Some(generation) {
            return;
        }
        status.available = false;
        status.malformed_frames += 1;
        status.last_error = Some(error.clone());
        let _ = self.events.send(ServerEvent::new(
            "ai_input_error",
            json!({"generation": generation, "error": error}),
        ));
    }
}

pub async fn read_ai_frame_ipc<R: AsyncRead + Unpin>(
    mut reader: R,
    hub: AiHub,
    generation: String,
) {
    let mut header = [0_u8; AIPF_HEADER_SIZE];
    loop {
        match reader.read_exact(&mut header).await {
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(error) => {
                hub.reader_error(&generation, format!("read AIPF header: {error}"));
                break;
            }
        }
        if &header[0..4] != AIPF_MAGIC || u16::from_be_bytes([header[4], header[5]]) != AIPF_VERSION
        {
            hub.reader_error(&generation, "invalid AIPF header".into());
            break;
        }
        let Some(fit_mode) = AiFitMode::from_wire(u16::from_be_bytes([header[6], header[7]]))
        else {
            hub.reader_error(&generation, "invalid AIPF fit mode".into());
            break;
        };
        let length = u32_at(&header, 8) as usize;
        if length == 0 || length > hub.max_frame_bytes {
            hub.reader_error(&generation, format!("invalid AIPF payload length {length}"));
            break;
        }
        let width = u32_at(&header, 28);
        let height = u32_at(&header, 32);
        let y_stride = u32_at(&header, 36);
        let uv_stride = u32_at(&header, 40);
        let height_stride = u32_at(&header, 44);
        let expected = y_stride as usize * height_stride as usize * 3 / 2;
        if width == 0 || height == 0 || y_stride < width || length < expected {
            hub.reader_error(&generation, "inconsistent AIPF dimensions".into());
            break;
        }
        let mut payload = vec![0_u8; length];
        if let Err(error) = reader.read_exact(&mut payload).await {
            hub.reader_error(&generation, format!("read AIPF payload: {error}"));
            break;
        }
        hub.ingest(AiFrame {
            generation: generation.clone(),
            pts: u64_at(&header, 12),
            sequence: u64_at(&header, 20),
            width,
            height,
            y_stride,
            uv_stride,
            height_stride,
            main_width: u32_at(&header, 48),
            main_height: u32_at(&header, 52),
            fit_mode,
            transform: AiFrameTransform {
                crop_x: i32_at(&header, 56),
                crop_y: i32_at(&header, 60),
                crop_width: i32_at(&header, 64),
                crop_height: i32_at(&header, 68),
                pad_left: i32_at(&header, 72),
                pad_top: i32_at(&header, 76),
                pad_right: i32_at(&header, 80),
                pad_bottom: i32_at(&header, 84),
            },
            data: Bytes::from(payload),
        });
    }
    hub.clear_generation(&generation);
}

fn u32_at(data: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes(data[offset..offset + 4].try_into().unwrap())
}

fn i32_at(data: &[u8], offset: usize) -> i32 {
    i32::from_be_bytes(data[offset..offset + 4].try_into().unwrap())
}

fn u64_at(data: &[u8], offset: usize) -> u64 {
    u64::from_be_bytes(data[offset..offset + 8].try_into().unwrap())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncWriteExt, duplex};

    fn message(payload: &[u8]) -> Vec<u8> {
        let mut data = Vec::new();
        data.extend_from_slice(AIPF_MAGIC);
        data.extend_from_slice(&AIPF_VERSION.to_be_bytes());
        data.extend_from_slice(&1_u16.to_be_bytes());
        data.extend_from_slice(&(payload.len() as u32).to_be_bytes());
        data.extend_from_slice(&42_u64.to_be_bytes());
        data.extend_from_slice(&7_u64.to_be_bytes());
        for value in [2_u32, 2, 2, 2, 2, 1920, 1080] {
            data.extend_from_slice(&value.to_be_bytes());
        }
        for value in [0_i32, 0, 1920, 1080, 0, 0, 0, 0] {
            data.extend_from_slice(&value.to_be_bytes());
        }
        data.extend_from_slice(payload);
        data
    }

    #[tokio::test]
    async fn reads_aipf_frame() {
        let (events, _) = broadcast::channel(8);
        let hub = AiHub::new(1024, events);
        hub.begin_generation("g1".into(), AiInputConfig::default());
        let (mut writer, reader) = duplex(16);
        let task = tokio::spawn(read_ai_frame_ipc(reader, hub.clone(), "g1".into()));
        let data = message(&[0; 6]);
        for chunk in data.chunks(3) {
            writer.write_all(chunk).await.unwrap();
        }
        tokio::time::sleep(std::time::Duration::from_millis(10)).await;
        assert_eq!(hub.status().frames_received, 1);
        drop(writer);
        task.await.unwrap();
    }
}
