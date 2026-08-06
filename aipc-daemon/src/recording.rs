use crate::config::RecordingConfig;
use crate::model::{ServerEvent, now_ms};
use crate::preview::{CodecConfig, PreviewFrame, VideoHub, annex_b_nals};
use anyhow::{Context, anyhow, bail};
use bytes::{Bytes, BytesMut};
use mp4::{AvcConfig, MediaConfig, Mp4Config, Mp4Sample, Mp4Writer, TrackConfig, TrackType};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokio::sync::{Mutex, RwLock, broadcast, oneshot};
use uuid::Uuid;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RecordingState {
    Idle,
    WaitingKeyframe,
    Recording,
    Finalizing,
    Failed,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RecordingStatus {
    pub state: RecordingState,
    pub id: Option<String>,
    pub file_name: Option<String>,
    pub generation: Option<String>,
    pub started_at_ms: Option<u64>,
    pub duration_ms: u64,
    pub bytes: u64,
    pub last_error: Option<String>,
}

impl Default for RecordingStatus {
    fn default() -> Self {
        Self {
            state: RecordingState::Idle,
            id: None,
            file_name: None,
            generation: None,
            started_at_ms: None,
            duration_ms: 0,
            bytes: 0,
            last_error: None,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RecordingEntry {
    pub id: String,
    pub file_name: String,
    pub created_at_ms: u64,
    pub duration_ms: u64,
    pub bytes: u64,
    pub width: i32,
    pub height: i32,
    pub fps: i32,
    pub generation: String,
    #[serde(default)]
    pub storage_directory: PathBuf,
}

#[derive(Debug, Clone, Serialize)]
pub struct RecordingList {
    pub items: Vec<RecordingEntry>,
    pub total: usize,
    pub offset: usize,
    pub limit: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RecordingSettingsUpdate {
    pub directory: PathBuf,
}

#[derive(Clone)]
pub struct RecordingManager {
    config: Arc<RwLock<RecordingConfig>>,
    hub: VideoHub,
    status: Arc<RwLock<RecordingStatus>>,
    entries: Arc<RwLock<Vec<RecordingEntry>>>,
    active: Arc<Mutex<Option<ActiveRecording>>>,
    index_path: PathBuf,
    settings_path: PathBuf,
    events: broadcast::Sender<ServerEvent>,
}

struct ActiveRecording {
    id: String,
    stop: Option<oneshot::Sender<()>>,
}

impl RecordingManager {
    pub async fn new(
        mut config: RecordingConfig,
        data_dir: &Path,
        hub: VideoHub,
        events: broadcast::Sender<ServerEvent>,
    ) -> anyhow::Result<Self> {
        let settings_path = data_dir.join("recording-settings.json");
        if let Ok(data) = tokio::fs::read(&settings_path).await {
            if let Ok(saved) = serde_json::from_slice::<RecordingSettingsUpdate>(&data) {
                config.directory = saved.directory;
            }
        }
        let index_path = data_dir.join("recordings.json");
        let mut entries = match tokio::fs::read(&index_path).await {
            Ok(data) => serde_json::from_slice(&data).unwrap_or_default(),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Vec::new(),
            Err(error) => return Err(error.into()),
        };
        entries.retain(|entry: &RecordingEntry| {
            let directory = if entry.storage_directory.as_os_str().is_empty() {
                config.directory.clone()
            } else {
                entry.storage_directory.clone()
            };
            directory.join(&entry.file_name).is_file()
        });
        Ok(Self {
            config: Arc::new(RwLock::new(config)),
            hub,
            status: Arc::new(RwLock::new(RecordingStatus::default())),
            entries: Arc::new(RwLock::new(entries)),
            active: Arc::new(Mutex::new(None)),
            index_path,
            settings_path,
            events,
        })
    }

    pub async fn settings(&self) -> RecordingConfig {
        self.config.read().await.clone()
    }

    pub async fn update_settings(
        &self,
        update: RecordingSettingsUpdate,
    ) -> anyhow::Result<RecordingConfig> {
        if self.active.lock().await.is_some() {
            bail!("cannot change recording directory while recording");
        }
        let mut next = self.config.read().await.clone();
        next.directory = update.directory;
        validate_directory(&next).await?;
        if let Some(parent) = self.settings_path.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        let data = serde_json::to_vec_pretty(&RecordingSettingsUpdate {
            directory: next.directory.clone(),
        })?;
        let temporary = self.settings_path.with_extension("json.tmp");
        tokio::fs::write(&temporary, data).await?;
        tokio::fs::rename(&temporary, &self.settings_path).await?;
        *self.config.write().await = next.clone();
        Ok(next)
    }

    pub async fn status(&self) -> RecordingStatus {
        self.status.read().await.clone()
    }

    pub async fn start(&self) -> anyhow::Result<RecordingStatus> {
        let config = self.config.read().await.clone();
        if !config.enabled {
            bail!("recording is disabled");
        }
        validate_directory(&config).await?;
        let codec = self
            .hub
            .codec_config()
            .ok_or_else(|| anyhow!("video stream is not ready"))?;
        let mut active = self.active.lock().await;
        if active.is_some() {
            bail!("recording is already active");
        }
        let id = Uuid::new_v4().to_string();
        let file_name = format!("recording-{}-{}.mp4", now_ms(), &id[..8]);
        let (stop, stop_rx) = oneshot::channel();
        *active = Some(ActiveRecording {
            id: id.clone(),
            stop: Some(stop),
        });
        let status = RecordingStatus {
            state: RecordingState::WaitingKeyframe,
            id: Some(id.clone()),
            file_name: Some(file_name.clone()),
            generation: Some(codec.info.generation.clone()),
            started_at_ms: Some(now_ms()),
            duration_ms: 0,
            bytes: 0,
            last_error: None,
        };
        *self.status.write().await = status.clone();
        self.emit(
            "waiting_keyframe",
            json!({"id": id, "file_name": file_name}),
        );
        let manager = self.clone();
        let receiver = self.hub.subscribe();
        tokio::spawn(async move {
            let result = run_recording(manager.clone(), config, codec, receiver, stop_rx).await;
            manager.finish(result).await;
        });
        Ok(status)
    }

    pub async fn stop(&self) -> anyhow::Result<RecordingStatus> {
        let mut active = self.active.lock().await;
        let Some(active_recording) = active.as_mut() else {
            bail!("recording is not active");
        };
        let stop = active_recording
            .stop
            .take()
            .ok_or_else(|| anyhow!("recording is already stopping"))?;
        let _ = stop.send(());
        self.status.write().await.state = RecordingState::Finalizing;
        Ok(self.status().await)
    }

    async fn finish(&self, result: anyhow::Result<RecordingEntry>) {
        self.active.lock().await.take();
        match result {
            Ok(entry) => {
                self.entries.write().await.insert(0, entry.clone());
                if let Err(error) = self.persist_index().await {
                    self.emit("index_error", json!({"message": error.to_string()}));
                }
                *self.status.write().await = RecordingStatus::default();
                self.emit("completed", json!({"recording": entry}));
            }
            Err(error) => {
                let mut status = self.status.write().await;
                status.state = RecordingState::Failed;
                status.last_error = Some(error.to_string());
                self.emit("failed", json!({"message": error.to_string()}));
            }
        }
    }

    pub async fn list(&self, offset: usize, limit: usize) -> RecordingList {
        let entries = self.entries.read().await;
        let limit = limit.clamp(1, 100);
        RecordingList {
            items: entries.iter().skip(offset).take(limit).cloned().collect(),
            total: entries.len(),
            offset,
            limit,
        }
    }

    pub async fn entry(&self, id: &str) -> Option<RecordingEntry> {
        self.entries
            .read()
            .await
            .iter()
            .find(|entry| entry.id == id)
            .cloned()
    }

    pub async fn path_for(&self, id: &str) -> anyhow::Result<(RecordingEntry, PathBuf)> {
        let entry = self
            .entry(id)
            .await
            .ok_or_else(|| anyhow!("recording not found"))?;
        let config = self.config.read().await;
        let directory = if entry.storage_directory.as_os_str().is_empty() {
            &config.directory
        } else {
            &entry.storage_directory
        };
        let path = directory.join(&entry.file_name);
        let canonical = tokio::fs::canonicalize(&path)
            .await
            .map_err(|error| anyhow!("recording file not found: {error}"))?;
        let directory = tokio::fs::canonicalize(directory).await?;
        let mut allowed = false;
        for root in &config.allowed_roots {
            if canonical.starts_with(tokio::fs::canonicalize(root).await?) {
                allowed = true;
                break;
            }
        }
        if !canonical.starts_with(&directory) || !allowed {
            bail!("recording path escaped managed directory");
        }
        Ok((entry, canonical))
    }

    pub async fn delete(&self, ids: &[String]) -> anyhow::Result<usize> {
        if let Some(active) = self.active.lock().await.as_ref() {
            if ids.iter().any(|id| id == &active.id) {
                bail!("cannot delete active recording");
            }
        }
        let mut deleted = 0;
        for id in ids {
            if let Ok((_, path)) = self.path_for(id).await {
                match tokio::fs::remove_file(path).await {
                    Ok(()) => deleted += 1,
                    Err(error) if error.kind() == std::io::ErrorKind::NotFound => deleted += 1,
                    Err(error) => return Err(error.into()),
                }
            }
        }
        self.entries
            .write()
            .await
            .retain(|entry| !ids.contains(&entry.id));
        self.persist_index().await?;
        Ok(deleted)
    }

    async fn persist_index(&self) -> anyhow::Result<()> {
        if let Some(parent) = self.index_path.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        let data = serde_json::to_vec_pretty(&*self.entries.read().await)?;
        let temporary = self.index_path.with_extension("json.tmp");
        tokio::fs::write(&temporary, data).await?;
        tokio::fs::rename(temporary, &self.index_path).await?;
        Ok(())
    }

    fn emit(&self, action: &str, payload: serde_json::Value) {
        let _ = self.events.send(ServerEvent::new(
            "recording",
            json!({"action": action, "payload": payload}),
        ));
    }

    pub async fn shutdown(&self) {
        if self.active.lock().await.is_some() {
            let _ = self.stop().await;
        }
        for _ in 0..100 {
            if self.active.lock().await.is_none() {
                return;
            }
            tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        }
    }
}

async fn validate_directory(config: &RecordingConfig) -> anyhow::Result<()> {
    if !config.directory.is_absolute() {
        bail!("recording directory must be absolute");
    }
    tokio::fs::create_dir_all(&config.directory).await?;
    let directory = tokio::fs::canonicalize(&config.directory).await?;
    let mut allowed = false;
    for root in &config.allowed_roots {
        tokio::fs::create_dir_all(root).await?;
        if directory.starts_with(tokio::fs::canonicalize(root).await?) {
            allowed = true;
            break;
        }
    }
    if !allowed {
        bail!("recording directory is outside configured allowed roots");
    }
    let free = free_bytes(&directory)?;
    if free < config.min_free_bytes {
        bail!("insufficient free space for recording");
    }
    Ok(())
}

fn free_bytes(path: &Path) -> anyhow::Result<u64> {
    use std::ffi::CString;
    use std::os::unix::ffi::OsStrExt;
    let value = CString::new(path.as_os_str().as_bytes())?;
    let mut stats = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    if unsafe { libc::statvfs(value.as_ptr(), stats.as_mut_ptr()) } != 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    let stats = unsafe { stats.assume_init() };
    Ok(stats.f_bavail as u64 * stats.f_frsize as u64)
}

async fn run_recording(
    manager: RecordingManager,
    config: RecordingConfig,
    codec: CodecConfig,
    mut receiver: broadcast::Receiver<Arc<PreviewFrame>>,
    mut stop: oneshot::Receiver<()>,
) -> anyhow::Result<RecordingEntry> {
    let status = manager.status().await;
    let id = status
        .id
        .clone()
        .ok_or_else(|| anyhow!("recording id missing"))?;
    let file_name = status
        .file_name
        .clone()
        .ok_or_else(|| anyhow!("recording name missing"))?;
    let final_path = config.directory.join(&file_name);
    let part_path = final_path.with_extension("mp4.part");
    let frame_duration = (90_000_u32 / codec.info.fps.max(1) as u32).max(1);
    let mut first_pts = None;
    let mut previous: Option<Arc<PreviewFrame>> = None;
    let mut writer = None;
    let mut payload_bytes = 0_u64;

    loop {
        let frame = tokio::select! {
            _ = &mut stop => break,
            result = receiver.recv() => match result {
                Ok(frame) => frame,
                Err(broadcast::error::RecvError::Lagged(count)) => bail!("recording queue lagged by {count} frames"),
                Err(broadcast::error::RecvError::Closed) => break,
            }
        };
        if frame.info.generation != codec.info.generation {
            break;
        }
        if writer.is_none() {
            if !frame.keyframe {
                continue;
            }
            let file = File::create(&part_path)
                .with_context(|| format!("create {}", part_path.display()))?;
            let mp4_config = Mp4Config {
                major_brand: "isom".parse()?,
                minor_version: 512,
                compatible_brands: vec![
                    "isom".parse()?,
                    "iso2".parse()?,
                    "avc1".parse()?,
                    "mp41".parse()?,
                ],
                timescale: 90_000,
            };
            let mut next = Mp4Writer::write_start(BufWriter::new(file), &mp4_config)?;
            next.add_track(&TrackConfig {
                track_type: TrackType::Video,
                timescale: 90_000,
                language: "und".into(),
                media_conf: MediaConfig::AvcConfig(AvcConfig {
                    width: codec.info.width as u16,
                    height: codec.info.height as u16,
                    seq_param_set: codec.sps.to_vec(),
                    pic_param_set: codec.pps.to_vec(),
                }),
            })?;
            first_pts = Some(frame.pts);
            manager.status.write().await.state = RecordingState::Recording;
            manager.emit("started", json!({"id": id, "file_name": file_name}));
            writer = Some(next);
            previous = Some(frame);
            continue;
        }
        if let Some(old) = previous.replace(frame.clone()) {
            let base = first_pts.unwrap_or(old.pts);
            let duration =
                pts_to_90k(frame.pts.saturating_sub(old.pts)).clamp(1, frame_duration * 10);
            let sample = Mp4Sample {
                start_time: pts_to_90k_u64(old.pts.saturating_sub(base)),
                duration,
                rendering_offset: 0,
                is_sync: old.keyframe,
                bytes: annex_b_to_avcc(&old.data)?,
            };
            payload_bytes = payload_bytes.saturating_add(sample.bytes.len() as u64);
            writer.as_mut().unwrap().write_sample(1, &sample)?;
            let duration_ms = old.pts.saturating_sub(base) / 1000;
            {
                let mut status = manager.status.write().await;
                status.duration_ms = duration_ms;
                status.bytes = payload_bytes;
            }
            if duration_ms / 1000 >= config.max_duration_sec
                || payload_bytes >= config.max_file_bytes
            {
                break;
            }
            if free_bytes(&config.directory)? < config.min_free_bytes {
                bail!("recording stopped because free space reserve was reached");
            }
        }
    }

    let mut writer =
        writer.ok_or_else(|| anyhow!("recording stopped before a keyframe arrived"))?;
    if let Some(last) = previous {
        let base = first_pts.unwrap_or(last.pts);
        let sample = Mp4Sample {
            start_time: pts_to_90k_u64(last.pts.saturating_sub(base)),
            duration: frame_duration,
            rendering_offset: 0,
            is_sync: last.keyframe,
            bytes: annex_b_to_avcc(&last.data)?,
        };
        payload_bytes += sample.bytes.len() as u64;
        writer.write_sample(1, &sample)?;
        let duration_ms =
            last.pts.saturating_sub(base) / 1000 + (frame_duration as u64 * 1000 / 90_000);
        let mut status = manager.status.write().await;
        status.duration_ms = duration_ms;
        status.bytes = payload_bytes;
    }
    writer.write_end()?;
    let mut output = writer.into_writer();
    output.flush()?;
    output.get_ref().sync_all()?;
    drop(output);
    tokio::fs::rename(&part_path, &final_path).await?;
    let metadata = tokio::fs::metadata(&final_path).await?;
    let final_status = manager.status().await;
    Ok(RecordingEntry {
        id,
        file_name,
        created_at_ms: final_status.started_at_ms.unwrap_or_else(now_ms),
        duration_ms: final_status.duration_ms,
        bytes: metadata.len().max(payload_bytes),
        width: codec.info.width,
        height: codec.info.height,
        fps: codec.info.fps,
        generation: codec.info.generation,
        storage_directory: config.directory,
    })
}

fn pts_to_90k(value_us: u64) -> u32 {
    pts_to_90k_u64(value_us).min(u32::MAX as u64) as u32
}

fn pts_to_90k_u64(value_us: u64) -> u64 {
    value_us.saturating_mul(90) / 1000
}

pub fn annex_b_to_avcc(data: &Bytes) -> anyhow::Result<Bytes> {
    let mut output = BytesMut::with_capacity(data.len());
    for (kind, nal) in annex_b_nals(data) {
        if matches!(kind, 7 | 8 | 9) {
            continue;
        }
        let payload = if nal.starts_with(&[0, 0, 0, 1]) {
            nal.slice(4..)
        } else if nal.starts_with(&[0, 0, 1]) {
            nal.slice(3..)
        } else {
            nal
        };
        if payload.is_empty() {
            continue;
        }
        output.extend_from_slice(&(payload.len() as u32).to_be_bytes());
        output.extend_from_slice(&payload);
    }
    if output.is_empty() {
        bail!("H264 access unit contained no media NAL units");
    }
    Ok(output.freeze())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::PreviewConfig;
    use crate::preview::StreamInfo;
    use std::time::Duration;
    use tempfile::tempdir;

    #[test]
    fn converts_annex_b_to_avcc_and_omits_parameter_sets() {
        let input = Bytes::from_static(&[
            0, 0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 2, 0, 0, 0, 1, 0x65, 3, 4,
        ]);
        let output = annex_b_to_avcc(&input).unwrap();
        assert_eq!(&output[..4], &[0, 0, 0, 3]);
        assert_eq!(&output[4..], &[0x65, 3, 4]);
    }

    #[test]
    fn converts_microseconds_to_90khz() {
        assert_eq!(pts_to_90k_u64(1_000_000), 90_000);
        assert_eq!(pts_to_90k(33_333), 2_999);
    }

    #[tokio::test]
    async fn records_a_playable_mp4_file() {
        let temp = tempdir().unwrap();
        let (events, _) = broadcast::channel(16);
        let hub = VideoHub::new(PreviewConfig::default(), events.clone());
        let info = StreamInfo {
            generation: "test-generation".into(),
            codec: "h264",
            format: "annexb",
            width: 640,
            height: 360,
            fps: 30,
        };
        hub.begin_generation(info.clone());
        hub.ingest(PreviewFrame {
            info: info.clone(),
            pts: 1_000_000,
            sequence: 1,
            keyframe: true,
            data: Bytes::from_static(&[
                0, 0, 0, 1, 0x67, 0x64, 0, 0x1f, 0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80, 0, 0, 0, 1,
                0x65, 1, 2, 3,
            ]),
        });
        let config = RecordingConfig {
            directory: temp.path().join("recordings"),
            allowed_roots: vec![temp.path().to_path_buf()],
            min_free_bytes: 0,
            ..RecordingConfig::default()
        };
        let manager = RecordingManager::new(config, temp.path(), hub.clone(), events)
            .await
            .unwrap();
        manager.start().await.unwrap();
        hub.ingest(PreviewFrame {
            info: info.clone(),
            pts: 1_033_333,
            sequence: 2,
            keyframe: true,
            data: Bytes::from_static(&[0, 0, 0, 1, 0x65, 4, 5, 6]),
        });
        hub.ingest(PreviewFrame {
            info,
            pts: 1_066_666,
            sequence: 3,
            keyframe: false,
            data: Bytes::from_static(&[0, 0, 0, 1, 0x41, 7, 8, 9]),
        });
        tokio::time::sleep(Duration::from_millis(40)).await;
        manager.stop().await.unwrap();
        for _ in 0..50 {
            if manager.list(0, 10).await.total == 1 {
                break;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        let list = manager.list(0, 10).await;
        assert_eq!(list.total, 1);
        let (_, path) = manager.path_for(&list.items[0].id).await.unwrap();
        let file = std::fs::File::open(path).unwrap();
        let size = file.metadata().unwrap().len();
        let reader = mp4::Mp4Reader::read_header(std::io::BufReader::new(file), size).unwrap();
        assert_eq!(reader.tracks().len(), 1);
    }
}
