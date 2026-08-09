use crate::ai::{AiHub, read_ai_frame_ipc};
use crate::config::{AiInputConfig, InputConfig, InputSourceConfig, InputSourceKind};
use crate::model::{ServerEvent, now_ms};
use crate::preview::{PreviewFrame, PreviewHub, StreamInfo};
use anyhow::{Context, bail};
use bytes::Bytes;
use futures_util::StreamExt;
use mp4::{Mp4Reader, TrackType};
use retina::client::{Credentials, Session, SessionOptions, SetupOptions, Transport};
use retina::codec::{CodecItem, FrameFormat, ParametersRef};
use serde::Serialize;
use serde_json::json;
use std::collections::BTreeMap;
use std::fs::File;
use std::io::Read;
use std::os::fd::{AsRawFd, RawFd};
use std::os::unix::net::UnixStream as StdUnixStream;
use std::path::Path;
use std::process::Stdio;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncWriteExt, BufReader};
use tokio::process::{Child, Command};
use tokio::sync::{Mutex, broadcast, mpsc, watch};
use tokio::task::JoinHandle;
use url::Url;
use uuid::Uuid;

const MAX_FILE_BYTES: u64 = 512 * 1024 * 1024;

#[derive(Debug, Clone, Serialize)]
pub struct SourceStatus {
    pub id: String,
    pub kind: &'static str,
    pub ai_sidecar: bool,
    pub state: String,
    pub generation: Option<String>,
    pub frames: u64,
    pub bytes: u64,
    pub last_pts: Option<u64>,
    pub last_error: Option<String>,
    pub updated_at_ms: u64,
}

impl SourceStatus {
    fn new(config: &InputSourceConfig) -> Self {
        Self {
            id: config.id.clone(),
            kind: match config.source {
                InputSourceKind::File { .. } => "file",
                InputSourceKind::Rtsp { .. } => "rtsp",
            },
            ai_sidecar: config.ai_sidecar,
            state: "stopped".into(),
            generation: None,
            frames: 0,
            bytes: 0,
            last_pts: None,
            last_error: None,
            updated_at_ms: now_ms(),
        }
    }
}

struct RunningSource {
    cancel: watch::Sender<bool>,
    task: JoinHandle<()>,
}

struct SourceInner {
    config: RwLock<InputConfig>,
    hub: PreviewHub,
    events: broadcast::Sender<ServerEvent>,
    ai: AiHub,
    statuses: RwLock<BTreeMap<String, SourceStatus>>,
    running: Mutex<Option<(String, RunningSource)>>,
    reconnect_initial: Duration,
    reconnect_max: Duration,
    processor_path: std::path::PathBuf,
    processor_config: AiInputConfig,
    processor_vdec_channel: i32,
    processor_vpss_group: i32,
}

#[derive(Clone)]
pub struct SourceManager {
    inner: Arc<SourceInner>,
}

impl SourceManager {
    pub fn new(
        config: InputConfig,
        hub: PreviewHub,
        ai: AiHub,
        events: broadcast::Sender<ServerEvent>,
    ) -> anyhow::Result<Self> {
        config.validate()?;
        let reconnect_initial = Duration::from_millis(config.reconnect_initial_ms);
        let reconnect_max = Duration::from_millis(config.reconnect_max_ms);
        let processor_path = config.processor_path.clone();
        let processor_config = config.processor.clone();
        let processor_vdec_channel = config.processor_vdec_channel;
        let processor_vpss_group = config.processor_vpss_group;
        let statuses = config
            .sources
            .iter()
            .map(|source| (source.id.clone(), SourceStatus::new(source)))
            .collect();
        Ok(Self {
            inner: Arc::new(SourceInner {
                config: RwLock::new(config),
                hub,
                events,
                ai,
                statuses: RwLock::new(statuses),
                running: Mutex::new(None),
                reconnect_initial,
                reconnect_max,
                processor_path,
                processor_config,
                processor_vdec_channel,
                processor_vpss_group,
            }),
        })
    }

    pub fn enabled(&self) -> bool {
        self.inner.config.read().unwrap().enabled
    }

    pub fn active_id(&self) -> Option<String> {
        self.inner.config.read().unwrap().active_source.clone()
    }

    pub fn list(&self) -> Vec<SourceStatus> {
        self.inner
            .statuses
            .read()
            .unwrap()
            .values()
            .cloned()
            .collect()
    }

    pub fn config(&self, id: &str) -> Option<InputSourceConfig> {
        self.inner
            .config
            .read()
            .unwrap()
            .sources
            .iter()
            .find(|source| source.id == id)
            .cloned()
    }

    pub async fn start_active(&self) -> anyhow::Result<()> {
        let Some(id) = self.active_id() else {
            return Ok(());
        };
        self.start(&id).await
    }

    pub async fn start(&self, id: &str) -> anyhow::Result<()> {
        let config = self
            .config(id)
            .ok_or_else(|| anyhow::anyhow!("input source {id} not found"))?;
        self.inner.config.write().unwrap().active_source = Some(id.to_owned());
        self.stop_running().await?;
        let generation = Uuid::new_v4().to_string();
        self.update_status(id, |status| {
            status.state = "starting".into();
            status.generation = Some(generation.clone());
            status.frames = 0;
            status.bytes = 0;
            status.last_pts = None;
            status.last_error = None;
        });
        let (cancel, cancel_rx) = watch::channel(false);
        let manager = self.clone();
        let source_id = id.to_owned();
        let task_generation = generation.clone();
        let task = tokio::spawn(async move {
            let result = run_source(
                config,
                source_id.clone(),
                task_generation.clone(),
                manager.clone(),
                cancel_rx,
            )
            .await;
            if let Err(error) = result {
                manager.update_status(&source_id, |status| {
                    status.state = "failed".into();
                    status.last_error = Some(error.to_string());
                });
                let _ = manager.inner.events.send(ServerEvent::new(
                    "source_error",
                    json!({"source_id": source_id, "generation": task_generation, "error": error.to_string()}),
                ));
            } else {
                manager.update_status(&source_id, |status| status.state = "stopped".into());
            }
            manager.inner.hub.stop_generation(&task_generation);
        });
        *self.inner.running.lock().await = Some((id.to_owned(), RunningSource { cancel, task }));
        let _ = self
            .inner
            .events
            .send(ServerEvent::new("source_started", json!({"source_id": id})));
        Ok(())
    }

    pub async fn stop(&self, id: &str) -> anyhow::Result<()> {
        let running_id = self
            .inner
            .running
            .lock()
            .await
            .as_ref()
            .map(|item| item.0.clone());
        if running_id.as_deref() == Some(id) {
            self.stop_running().await?;
        }
        self.update_status(id, |status| status.state = "stopped".into());
        Ok(())
    }

    pub async fn reconnect(&self, id: &str) -> anyhow::Result<()> {
        self.start(id).await
    }

    pub async fn set_active(&self, id: &str) -> anyhow::Result<()> {
        anyhow::ensure!(self.config(id).is_some(), "input source {id} not found");
        self.inner.config.write().unwrap().active_source = Some(id.to_owned());
        self.start(id).await
    }

    pub async fn upsert(&self, source: InputSourceConfig) -> anyhow::Result<()> {
        let source_id = source.id.clone();
        let status_source = source.clone();
        let mut config = self.inner.config.read().unwrap().clone();
        if let Some(existing) = config.sources.iter_mut().find(|item| item.id == source.id) {
            *existing = source;
        } else {
            config.sources.push(source);
        }
        config.validate()?;
        self.inner.config.write().unwrap().clone_from(&config);
        self.inner
            .statuses
            .write()
            .unwrap()
            .entry(source_id)
            .and_modify(|status| {
                status.kind = match status_source.source {
                    InputSourceKind::File { .. } => "file",
                    InputSourceKind::Rtsp { .. } => "rtsp",
                };
                status.ai_sidecar = status_source.ai_sidecar;
                status.updated_at_ms = now_ms();
            })
            .or_insert_with(|| SourceStatus::new(&status_source));
        Ok(())
    }

    pub async fn shutdown(&self) {
        let _ = self.stop_running().await;
    }

    async fn stop_running(&self) -> anyhow::Result<()> {
        let running = self.inner.running.lock().await.take();
        let Some((id, running)) = running else {
            return Ok(());
        };
        let _ = running.cancel.send(true);
        let _ = tokio::time::timeout(Duration::from_secs(2), running.task).await;
        self.update_status(&id, |status| status.state = "stopped".into());
        Ok(())
    }

    fn update_status<F>(&self, id: &str, update: F)
    where
        F: FnOnce(&mut SourceStatus),
    {
        if let Some(status) = self.inner.statuses.write().unwrap().get_mut(id) {
            update(status);
            status.updated_at_ms = now_ms();
        }
    }

    fn publish(&self, source_id: &str, frame: PreviewFrame) {
        self.update_status(source_id, |status| {
            status.state = "running".into();
            status.frames = status.frames.saturating_add(1);
            status.bytes = status.bytes.saturating_add(frame.data.len() as u64);
            status.last_pts = Some(frame.pts);
        });
        self.inner.hub.ingest(frame);
    }
}

#[derive(Clone)]
struct EncodedFileFrame {
    data: Bytes,
    pts: u64,
    keyframe: bool,
}

struct EncodedFileStream {
    frames: Vec<EncodedFileFrame>,
    width: i32,
    height: i32,
    fps: i32,
}

const AIPV2_HEADER_SIZE: usize = 32;
const AIPV2_VERSION: u16 = 2;
const AIPV2_KEYFRAME: u16 = 1;
const AIPV2_DISCONTINUITY: u16 = 1 << 1;
const AIPV2_CONFIG: u16 = 1 << 2;
const AIPV2_EOS: u16 = 1 << 3;

#[derive(Clone)]
struct EncodedAccessUnit {
    pts: u64,
    sequence: u64,
    keyframe: bool,
    discontinuity: bool,
    eos: bool,
    data: Bytes,
}

fn encode_aipv2(frame: &EncodedAccessUnit) -> Vec<u8> {
    let mut flags = 0_u16;
    if frame.keyframe {
        flags |= AIPV2_KEYFRAME;
    }
    if frame.discontinuity {
        flags |= AIPV2_DISCONTINUITY;
    }
    if frame.eos {
        flags |= AIPV2_EOS;
    }
    if crate::preview::annex_b_nals(&frame.data)
        .iter()
        .any(|(kind, _)| matches!(kind, 7 | 8))
    {
        flags |= AIPV2_CONFIG;
    }
    let mut output = Vec::with_capacity(AIPV2_HEADER_SIZE + frame.data.len());
    output.extend_from_slice(b"AIPV");
    output.extend_from_slice(&AIPV2_VERSION.to_be_bytes());
    output.extend_from_slice(&flags.to_be_bytes());
    output.extend_from_slice(&(frame.data.len() as u32).to_be_bytes());
    output.extend_from_slice(&frame.pts.to_be_bytes());
    output.extend_from_slice(&frame.sequence.to_be_bytes());
    output.extend_from_slice(&0_u32.to_be_bytes());
    output.extend_from_slice(&frame.data);
    output
}

struct ProcessorRuntime {
    sender: mpsc::Sender<EncodedAccessUnit>,
    dropping_until_keyframe: Arc<AtomicBool>,
    child: Arc<Mutex<Option<Child>>>,
    control: Arc<Mutex<tokio::net::UnixStream>>,
    generation: String,
    ai: AiHub,
}

impl ProcessorRuntime {
    async fn spawn(
        manager: &SourceManager,
        source_id: &str,
        generation: &str,
        info: &StreamInfo,
    ) -> anyhow::Result<Self> {
        let (input_parent, input_child) = StdUnixStream::pair()?;
        let (output_parent, output_child) = StdUnixStream::pair()?;
        let (processed_parent, processed_child) = StdUnixStream::pair()?;
        let (control_parent, control_child) = StdUnixStream::pair()?;
        input_parent.set_nonblocking(true)?;
        output_parent.set_nonblocking(true)?;
        processed_parent.set_nonblocking(true)?;
        control_parent.set_nonblocking(true)?;
        let input_fd = input_child.as_raw_fd();
        let output_fd = output_child.as_raw_fd();
        let processed_fd = processed_child.as_raw_fd();
        let control_fd = control_child.as_raw_fd();
        let mut command = Command::new(&manager.inner.processor_path);
        command
            .arg("--input-fd")
            .arg("3")
            .arg("--output-fd")
            .arg("4")
            .arg("--processed-output-fd")
            .arg("5")
            .arg("--control-fd")
            .arg("6")
            .arg("--source-id")
            .arg(source_id)
            .arg("--generation")
            .arg(generation)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true);
        unsafe {
            command
                .pre_exec(move || map_processor_fds(input_fd, output_fd, processed_fd, control_fd));
        }
        let mut child = command.spawn().with_context(|| {
            format!(
                "spawn video processor {}",
                manager.inner.processor_path.display()
            )
        })?;
        drop(input_child);
        drop(output_child);
        drop(processed_child);
        drop(processed_parent);
        drop(control_child);
        if let Some(stdout) = child.stdout.take() {
            spawn_processor_log_reader(
                stdout,
                manager.inner.events.clone(),
                source_id.to_owned(),
                generation.to_owned(),
                false,
            );
        }
        if let Some(stderr) = child.stderr.take() {
            spawn_processor_log_reader(
                stderr,
                manager.inner.events.clone(),
                source_id.to_owned(),
                generation.to_owned(),
                true,
            );
        }

        let mut control = tokio::net::UnixStream::from_std(control_parent)?;
        let config = json!({
            "version": 1,
            "command": "configure",
            "source_id": source_id,
            "source_generation": generation,
            "input": {
                "codec": info.codec,
                "format": info.format,
                "width": info.width,
                "height": info.height,
                "fps": info.fps,
            },
            "output": manager.inner.processor_config.clone(),
            "vdec_channel": manager.inner.processor_vdec_channel,
            "vpss_group": manager.inner.processor_vpss_group,
        });
        let payload = serde_json::to_vec(&config)?;
        control
            .write_all(&(payload.len() as u32).to_be_bytes())
            .await?;
        control.write_all(&payload).await?;
        let control = Arc::new(Mutex::new(control));

        let mut input = tokio::net::UnixStream::from_std(input_parent)?;
        let (sender, mut receiver) = mpsc::channel::<EncodedAccessUnit>(64);
        let events = manager.inner.events.clone();
        let event_source_id = source_id.to_owned();
        tokio::spawn(async move {
            while let Some(frame) = receiver.recv().await {
                if let Err(error) = input.write_all(&encode_aipv2(&frame)).await {
                    let _ = events.send(ServerEvent::new(
                        "source_processor_error",
                        json!({"source_id": event_source_id, "error": error.to_string()}),
                    ));
                    break;
                }
            }
        });

        let output = tokio::net::UnixStream::from_std(output_parent)?;
        let ai = manager.inner.ai.clone();
        ai.begin_generation_for_source(
            generation.to_owned(),
            manager.inner.processor_config.clone(),
            source_id.to_owned(),
        );
        tokio::spawn(read_ai_frame_ipc(output, ai.clone(), generation.to_owned()));

        let child = Arc::new(Mutex::new(Some(child)));
        Ok(Self {
            sender,
            // A live RTSP session can begin on a P-frame. Feeding undecodable
            // inter frames into VDEC before SPS/PPS + IDR may leave the
            // channel stalled, so every new processor generation starts in
            // keyframe-wait mode and resets VDEC on the first usable AU.
            dropping_until_keyframe: Arc::new(AtomicBool::new(true)),
            child,
            control,
            generation: generation.to_owned(),
            ai,
        })
    }

    fn push(&self, frame: EncodedAccessUnit) {
        let mut frame = frame;
        if self.dropping_until_keyframe.load(Ordering::Acquire) {
            if !frame.keyframe {
                return;
            }
            frame.discontinuity = true;
            self.dropping_until_keyframe.store(false, Ordering::Release);
        }
        if self.sender.try_send(frame).is_err() {
            self.dropping_until_keyframe.store(true, Ordering::Release);
        }
    }

    async fn stop(self) {
        let stop = serde_json::to_vec(&json!({"version": 1, "command": "stop"}));
        let stop_sent = if let Ok(payload) = stop {
            let mut control = self.control.lock().await;
            let header_written = control
                .write_all(&(payload.len() as u32).to_be_bytes())
                .await;
            header_written.is_ok() && control.write_all(&payload).await.is_ok()
        } else {
            false
        };
        if !stop_sent {
            let _ = self
                .sender
                .send(EncodedAccessUnit {
                    pts: 0,
                    sequence: 0,
                    keyframe: false,
                    discontinuity: false,
                    eos: true,
                    data: Bytes::new(),
                })
                .await;
        }
        drop(self.sender);
        if let Some(mut child) = self.child.lock().await.take() {
            if tokio::time::timeout(Duration::from_millis(500), child.wait())
                .await
                .is_err()
            {
                let _ = child.start_kill();
                let _ = tokio::time::timeout(Duration::from_secs(2), child.wait()).await;
            }
        }
        self.ai.clear_generation(&self.generation);
    }
}

fn map_processor_fds(
    input: RawFd,
    output: RawFd,
    processed: RawFd,
    control: RawFd,
) -> std::io::Result<()> {
    if unsafe { libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) } != 0 {
        return Err(std::io::Error::last_os_error());
    }
    let mappings = [(input, 3), (output, 4), (processed, 5), (control, 6)];
    let mut temporary = [-1_i32; 4];
    for (index, (source, _)) in mappings.iter().enumerate() {
        temporary[index] =
            unsafe { libc::fcntl(*source, libc::F_DUPFD_CLOEXEC, 10 + index as i32) };
        if temporary[index] < 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    for (index, (_, target)) in mappings.iter().enumerate() {
        if unsafe { libc::dup2(temporary[index], *target) } < 0 {
            return Err(std::io::Error::last_os_error());
        }
        unsafe { libc::close(temporary[index]) };
    }
    Ok(())
}

fn spawn_processor_log_reader<R>(
    reader: R,
    events: broadcast::Sender<ServerEvent>,
    source_id: String,
    generation: String,
    stderr: bool,
) where
    R: AsyncRead + Unpin + Send + 'static,
{
    tokio::spawn(async move {
        let mut lines = BufReader::new(reader).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            let payload = if stderr {
                json!({
                    "source_id": source_id,
                    "generation": generation,
                    "stream": "stderr",
                    "line": line,
                })
            } else {
                match serde_json::from_str::<serde_json::Value>(&line) {
                    Ok(value) => json!({
                        "source_id": source_id,
                        "generation": generation,
                        "event": value,
                    }),
                    Err(_) => json!({
                        "source_id": source_id,
                        "generation": generation,
                        "stream": "stdout",
                        "line": line,
                    }),
                }
            };
            let _ = events.send(ServerEvent::new("source_processor_event", payload));
        }
    });
}

async fn run_source(
    config: InputSourceConfig,
    source_id: String,
    generation: String,
    manager: SourceManager,
    cancel: watch::Receiver<bool>,
) -> anyhow::Result<()> {
    match config.source {
        InputSourceKind::File {
            path,
            loop_playback,
            fps,
            realtime,
        } => {
            run_file(
                path,
                loop_playback,
                fps,
                realtime,
                config.ai_sidecar,
                source_id,
                generation,
                manager,
                cancel,
            )
            .await
        }
        InputSourceKind::Rtsp {
            url,
            username,
            password,
            max_width,
            max_height,
            max_fps,
        } => {
            run_rtsp(
                url,
                username,
                password,
                max_width,
                max_height,
                max_fps,
                config.ai_sidecar,
                source_id,
                generation,
                manager,
                cancel,
            )
            .await
        }
    }
}

async fn run_file(
    path: std::path::PathBuf,
    loop_playback: bool,
    fps: u32,
    realtime: bool,
    ai_sidecar: bool,
    source_id: String,
    generation: String,
    manager: SourceManager,
    mut cancel: watch::Receiver<bool>,
) -> anyhow::Result<()> {
    let file_roots = manager.inner.config.read().unwrap().file_roots.clone();
    let stream = tokio::task::spawn_blocking(move || {
        let canonical = path
            .canonicalize()
            .with_context(|| format!("resolve input file {}", path.display()))?;
        let allowed = file_roots.iter().any(|root| {
            root.canonicalize()
                .is_ok_and(|canonical_root| canonical.starts_with(canonical_root))
        });
        anyhow::ensure!(allowed, "input file is outside configured file roots");
        read_file_stream(&canonical, fps)
    })
    .await
    .context("join file reader")??;
    let mut loop_index = 0_u64;
    loop {
        let loop_generation = if loop_index == 0 {
            generation.clone()
        } else {
            Uuid::new_v4().to_string()
        };
        manager.update_status(&source_id, |status| {
            status.generation = Some(loop_generation.clone());
            status.state = "starting".into();
        });
        let info = StreamInfo {
            generation: loop_generation.clone(),
            codec: "h264",
            format: "annexb",
            width: stream.width,
            height: stream.height,
            fps: stream.fps,
        };
        manager.inner.hub.begin_generation(info.clone());
        let processor =
            start_optional_processor(ai_sidecar, &manager, &source_id, &loop_generation, &info)
                .await;
        let started = Instant::now();
        for (sequence, frame) in stream.frames.iter().enumerate() {
            if realtime {
                let deadline = started + Duration::from_micros(frame.pts);
                tokio::select! {
                    _ = tokio::time::sleep_until(deadline.into()) => {}
                    _ = cancel.changed() => {
                        manager.inner.hub.stop_generation(&loop_generation);
                        return Ok(())
                    },
                }
            } else if *cancel.borrow() {
                manager.inner.hub.stop_generation(&loop_generation);
                return Ok(());
            }
            let encoded = EncodedAccessUnit {
                pts: frame.pts,
                sequence: sequence as u64,
                keyframe: frame.keyframe,
                discontinuity: false,
                eos: false,
                data: frame.data.clone(),
            };
            if let Some(processor) = &processor {
                processor.push(encoded);
            }
            manager.publish(
                &source_id,
                PreviewFrame {
                    info: info.clone(),
                    pts: frame.pts,
                    sequence: sequence as u64,
                    keyframe: frame.keyframe,
                    data: frame.data.clone(),
                },
            );
        }
        if let Some(processor) = processor {
            processor.stop().await;
        }
        manager.inner.hub.stop_generation(&loop_generation);
        if !loop_playback {
            let _ = manager.inner.events.send(ServerEvent::new(
                "source_eos",
                json!({"source_id": source_id, "generation": loop_generation}),
            ));
            break;
        }
        loop_index = loop_index.saturating_add(1);
    }
    Ok(())
}

async fn run_rtsp(
    raw_url: String,
    username: Option<String>,
    password: Option<String>,
    max_width: u32,
    max_height: u32,
    max_fps: u32,
    ai_sidecar: bool,
    source_id: String,
    generation: String,
    manager: SourceManager,
    mut cancel: watch::Receiver<bool>,
) -> anyhow::Result<()> {
    let (url, credentials) = rtsp_url_and_credentials(&raw_url, username, password)?;
    let mut retry = manager.inner.reconnect_initial;
    let mut attempt = 0_u64;
    loop {
        let attempt_generation = if attempt == 0 {
            generation.clone()
        } else {
            Uuid::new_v4().to_string()
        };
        manager.update_status(&source_id, |status| {
            status.state = if attempt == 0 {
                "starting"
            } else {
                "reconnecting"
            }
            .into();
            status.generation = Some(attempt_generation.clone());
            status.last_error = None;
        });
        let result = tokio::select! {
            _ = cancel.changed() => {
                manager.inner.hub.stop_generation(&attempt_generation);
                return Ok(())
            },
            result = run_rtsp_session(
                url.clone(),
                credentials.clone(),
                max_width,
                max_height,
                max_fps,
                source_id.clone(),
                attempt_generation.clone(),
                ai_sidecar,
                manager.clone(),
            ) => result,
        };
        manager.inner.hub.stop_generation(&attempt_generation);
        let error = match result {
            Ok(()) => "RTSP stream ended".to_owned(),
            Err(error) => sanitize_rtsp_error(&error.to_string(), &raw_url),
        };
        manager.update_status(&source_id, |status| {
            status.state = "backoff".into();
            status.last_error = Some(error.clone());
        });
        let _ = manager.inner.events.send(ServerEvent::new(
            "source_reconnecting",
            json!({
                "source_id": source_id,
                "generation": attempt_generation,
                "retry_in_ms": retry.as_millis() as u64,
                "error": error,
            }),
        ));
        tokio::select! {
            _ = tokio::time::sleep(retry) => {}
            _ = cancel.changed() => return Ok(()),
        }
        retry = retry.saturating_mul(2).min(manager.inner.reconnect_max);
        attempt = attempt.saturating_add(1);
    }
}

async fn run_rtsp_session(
    url: Url,
    credentials: Option<Credentials>,
    max_width: u32,
    max_height: u32,
    max_fps: u32,
    source_id: String,
    generation: String,
    ai_sidecar: bool,
    manager: SourceManager,
) -> anyhow::Result<()> {
    let options = SessionOptions::default().creds(credentials);
    let mut session = Session::describe(url, options)
        .await
        .context("RTSP DESCRIBE")?;
    let stream_index = session
        .streams()
        .iter()
        .position(|stream| stream.media() == "video" && stream.encoding_name() == "h264")
        .ok_or_else(|| anyhow::anyhow!("RTSP source has no H264 video stream"))?;
    let sdp_fps = session.streams()[stream_index].framerate();
    let setup = SetupOptions::default()
        .transport(Transport::Tcp(Default::default()))
        .frame_format(FrameFormat::SIMPLE);
    session
        .setup(stream_index, setup)
        .await
        .context("RTSP SETUP")?;
    let mut demuxed = session
        .play(Default::default())
        .await
        .context("RTSP PLAY")?
        .demuxed()?;
    let video = match demuxed.streams()[stream_index].parameters() {
        Some(ParametersRef::Video(video)) => video,
        _ => bail!("RTSP H264 parameters are unavailable"),
    };
    let (width, height) = video.pixel_dimensions();
    let fps = video
        .frame_rate()
        .map(|(num, den)| (den as f64 / num.max(1) as f64).round() as i32)
        .or_else(|| Some(sdp_fps.unwrap_or(25.0).round() as i32))
        .unwrap_or(25)
        .clamp(1, 120);
    anyhow::ensure!(
        width <= max_width && height <= max_height && fps <= max_fps as i32,
        "RTSP stream exceeds configured resolution/FPS limits"
    );
    let info = StreamInfo {
        generation,
        codec: "h264",
        format: "annexb",
        width: width as i32,
        height: height as i32,
        fps,
    };
    manager.inner.hub.begin_generation(info.clone());
    let processor =
        start_optional_processor(ai_sidecar, &manager, &source_id, &info.generation, &info).await;
    let mut sequence = 0_u64;
    loop {
        tokio::select! {
            item = demuxed.next() => match item {
                Some(Ok(CodecItem::VideoFrame(frame))) => {
                    sequence = sequence.saturating_add(1);
                    let pts = (frame.timestamp().elapsed_secs().max(0.0) * 1_000_000.0) as u64;
                    let data = Bytes::copy_from_slice(frame.data());
                    if let Some(processor) = &processor {
                        processor.push(EncodedAccessUnit {
                            pts,
                            sequence,
                            keyframe: frame.is_random_access_point(),
                            discontinuity: false,
                            eos: false,
                            data: data.clone(),
                        });
                    }
                    manager.publish(&source_id, PreviewFrame {
                        info: info.clone(), pts, sequence,
                        keyframe: frame.is_random_access_point(),
                        data,
                    });
                }
                Some(Ok(_)) => {}
                Some(Err(error)) => {
                    if let Some(processor) = processor { processor.stop().await; }
                    return Err(anyhow::anyhow!(error.to_string()));
                }
                None => {
                    if let Some(processor) = processor { processor.stop().await; }
                    return Ok(())
                }
            }
        }
    }
}

async fn start_optional_processor(
    ai_sidecar: bool,
    manager: &SourceManager,
    source_id: &str,
    generation: &str,
    info: &StreamInfo,
) -> Option<ProcessorRuntime> {
    if !ai_sidecar || !manager.inner.processor_config.enabled {
        return None;
    }
    match ProcessorRuntime::spawn(manager, source_id, generation, info).await {
        Ok(processor) => Some(processor),
        Err(error) => {
            let _ = manager.inner.events.send(ServerEvent::new(
                "source_processor_error",
                json!({
                    "source_id": source_id,
                    "generation": generation,
                    "error": error.to_string(),
                }),
            ));
            None
        }
    }
}

fn rtsp_url_and_credentials(
    raw_url: &str,
    username: Option<String>,
    password: Option<String>,
) -> anyhow::Result<(Url, Option<Credentials>)> {
    let mut url = Url::parse(raw_url).context("parse RTSP URL")?;
    anyhow::ensure!(url.scheme() == "rtsp", "RTSP URL must use rtsp://");
    let embedded_username = (!url.username().is_empty()).then(|| url.username().to_owned());
    let embedded_password = url.password().map(str::to_owned);
    let credentials = username.or(embedded_username).map(|username| Credentials {
        username,
        password: password.or(embedded_password).unwrap_or_default(),
    });
    url.set_username("")
        .map_err(|_| anyhow::anyhow!("invalid RTSP username"))?;
    url.set_password(None)
        .map_err(|_| anyhow::anyhow!("invalid RTSP password"))?;
    Ok((url, credentials))
}

fn sanitize_rtsp_error(error: &str, raw_url: &str) -> String {
    let redacted = Url::parse(raw_url)
        .ok()
        .map(|mut url| {
            if !url.username().is_empty() {
                let _ = url.set_username("***");
            }
            if url.password().is_some() {
                let _ = url.set_password(Some("***"));
            }
            url.to_string()
        })
        .unwrap_or_else(|| "<redacted-rtsp-url>".into());
    let mut message = error.replace(raw_url, &redacted);
    if message.len() > 512 {
        message.truncate(512);
    }
    message
}

fn read_file_stream(path: &Path, fps: u32) -> anyhow::Result<EncodedFileStream> {
    let metadata =
        std::fs::metadata(path).with_context(|| format!("stat input file {}", path.display()))?;
    anyhow::ensure!(
        metadata.len() <= MAX_FILE_BYTES,
        "input file exceeds 512 MiB"
    );
    let extension = path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or_default()
        .to_ascii_lowercase();
    if extension == "mp4" {
        return read_mp4_stream(path);
    }
    let mut data = Vec::with_capacity(metadata.len() as usize);
    File::open(path)?.read_to_end(&mut data)?;
    let frames = parse_annex_b_frames(&data, fps);
    anyhow::ensure!(!frames.is_empty(), "Annex-B input contains no access units");
    let (width, height) = annex_b_dimensions(&data).unwrap_or((0, 0));
    Ok(EncodedFileStream {
        frames,
        width: width as i32,
        height: height as i32,
        fps: fps as i32,
    })
}

fn read_mp4_stream(path: &Path) -> anyhow::Result<EncodedFileStream> {
    let file = File::open(path)?;
    let size = file.metadata()?.len();
    let mut reader = Mp4Reader::read_header(file, size).context("read MP4 header")?;
    let (track_id, timescale, width, height, track_fps, nal_length_size, sps, pps) = {
        let track = reader
            .tracks()
            .values()
            .find(|track| {
                track.track_type().ok() == Some(TrackType::Video)
                    && track
                        .media_type()
                        .ok()
                        .is_some_and(|kind| kind == mp4::MediaType::H264)
            })
            .ok_or_else(|| anyhow::anyhow!("MP4 contains no H264 video track"))?;
        (
            track.track_id(),
            track.timescale().max(1) as u64,
            track.width() as i32,
            track.height() as i32,
            track.frame_rate().round().clamp(1.0, 120.0) as i32,
            track
                .trak
                .mdia
                .minf
                .stbl
                .stsd
                .avc1
                .as_ref()
                .map(|avc1| (avc1.avcc.length_size_minus_one & 0x03) as usize + 1)
                .unwrap_or(4),
            track
                .sequence_parameter_set()
                .context("read MP4 SPS")?
                .to_vec(),
            track
                .picture_parameter_set()
                .context("read MP4 PPS")?
                .to_vec(),
        )
    };
    let count = reader.sample_count(track_id)?;
    let mut frames = Vec::with_capacity(count as usize);
    for sample_id in 1..=count {
        let Some(sample) = reader.read_sample(track_id, sample_id)? else {
            continue;
        };
        let mut data = avcc_to_annex_b_with_length(&sample.bytes, nal_length_size)?;
        if sample.is_sync {
            let mut bootstrap = Vec::with_capacity(sps.len() + pps.len() + data.len() + 8);
            append_annex_b_nal(&mut bootstrap, &sps);
            append_annex_b_nal(&mut bootstrap, &pps);
            bootstrap.append(&mut data);
            data = bootstrap;
        }
        let composition_time = sample.start_time as i128 + sample.rendering_offset as i128;
        frames.push(EncodedFileFrame {
            data: Bytes::from(data),
            pts: composition_time.max(0) as u64 * 1_000_000 / timescale,
            keyframe: sample.is_sync,
        });
    }
    anyhow::ensure!(!frames.is_empty(), "MP4 H264 track has no samples");
    frames.sort_by_key(|frame| frame.pts);
    let fallback_duration = 1_000_000 / track_fps.max(1) as u64;
    let _duration_us = frames
        .last()
        .map(|frame| frame.pts.saturating_add(fallback_duration))
        .unwrap_or(fallback_duration);
    Ok(EncodedFileStream {
        frames,
        width,
        height,
        fps: track_fps,
    })
}

fn append_annex_b_nal(output: &mut Vec<u8>, nal: &[u8]) {
    output.extend_from_slice(&[0, 0, 0, 1]);
    output.extend_from_slice(nal);
}

fn annex_b_dimensions(data: &[u8]) -> Option<(u32, u32)> {
    let bytes = Bytes::copy_from_slice(data);
    let mut sps = None;
    let mut pps = None;
    for (kind, nal) in crate::preview::annex_b_nals(&bytes) {
        match kind {
            // `annex_b_nals` intentionally returns the start code together
            // with each NAL for republishing. Retina's parameter parser,
            // however, expects a single NAL beginning at its one-byte header.
            7 => sps = strip_annex_b_prefix(&nal).map(Bytes::copy_from_slice),
            8 => pps = strip_annex_b_prefix(&nal).map(Bytes::copy_from_slice),
            _ => {}
        }
    }
    let parameters = retina::codec::h264::parameters_from_sps_and_pps(
        sps?.as_ref(),
        pps?.as_ref(),
        retina::codec::h26x::Framing::AnnexB,
    )
    .ok()?;
    Some(parameters.pixel_dimensions())
}

fn strip_annex_b_prefix(nal: &[u8]) -> Option<&[u8]> {
    if nal.starts_with(&[0, 0, 0, 1]) {
        Some(&nal[4..])
    } else if nal.starts_with(&[0, 0, 1]) {
        Some(&nal[3..])
    } else {
        None
    }
}

fn avcc_to_annex_b_with_length(data: &[u8], nal_length_size: usize) -> anyhow::Result<Vec<u8>> {
    anyhow::ensure!(
        (1..=4).contains(&nal_length_size),
        "invalid AVCC NAL length size"
    );
    let mut output = Vec::with_capacity(data.len() + 16);
    let mut offset = 0;
    while offset < data.len() {
        anyhow::ensure!(
            data.len() - offset >= nal_length_size,
            "truncated MP4 NAL length"
        );
        let mut length = 0_usize;
        for byte in &data[offset..offset + nal_length_size] {
            length = (length << 8) | *byte as usize;
        }
        offset += nal_length_size;
        anyhow::ensure!(
            length > 0 && length <= data.len() - offset,
            "invalid MP4 NAL length"
        );
        output.extend_from_slice(&[0, 0, 0, 1]);
        output.extend_from_slice(&data[offset..offset + length]);
        offset += length;
    }
    Ok(output)
}

fn parse_annex_b_frames(data: &[u8], fps: u32) -> Vec<EncodedFileFrame> {
    let nals = crate::preview::annex_b_nals(&Bytes::copy_from_slice(data));
    let frame_duration = 1_000_000 / fps.max(1) as u64;
    let mut frames = Vec::new();
    let mut current = Vec::new();
    let mut has_vcl = false;
    for (nal_type, nal) in nals {
        if has_vcl && matches!(nal_type, 6..=9) {
            frames.push(make_file_frame(
                &current,
                frames.len() as u64 * frame_duration,
            ));
            current.clear();
            has_vcl = false;
        }
        if (1..=5).contains(&nal_type) {
            if has_vcl && first_mb_in_slice(&nal) == Some(0) {
                frames.push(make_file_frame(
                    &current,
                    frames.len() as u64 * frame_duration,
                ));
                current.clear();
            }
            has_vcl = true;
        }
        current.extend_from_slice(&nal);
    }
    if !current.is_empty() {
        frames.push(make_file_frame(
            &current,
            frames.len() as u64 * frame_duration,
        ));
    }
    frames
}

fn first_mb_in_slice(nal: &[u8]) -> Option<u64> {
    let prefix = if nal.starts_with(&[0, 0, 0, 1]) {
        4
    } else if nal.starts_with(&[0, 0, 1]) {
        3
    } else {
        0
    };
    let payload = nal.get(prefix + 1..)?;
    let mut rbsp = Vec::with_capacity(payload.len());
    let mut zeros = 0;
    for &byte in payload {
        if zeros >= 2 && byte == 3 {
            zeros = 0;
            continue;
        }
        rbsp.push(byte);
        zeros = if byte == 0 { zeros + 1 } else { 0 };
    }
    let mut bit = 0_usize;
    let mut leading_zeros = 0_usize;
    while bit < rbsp.len() * 8 && read_bit(&rbsp, bit) == 0 {
        leading_zeros += 1;
        bit += 1;
        if leading_zeros > 63 {
            return None;
        }
    }
    if bit >= rbsp.len() * 8 {
        return None;
    }
    bit += 1;
    let mut suffix = 0_u64;
    for _ in 0..leading_zeros {
        if bit >= rbsp.len() * 8 {
            return None;
        }
        suffix = (suffix << 1) | read_bit(&rbsp, bit) as u64;
        bit += 1;
    }
    Some(((1_u64 << leading_zeros) - 1).saturating_add(suffix))
}

fn read_bit(data: &[u8], bit: usize) -> u8 {
    (data[bit / 8] >> (7 - (bit % 8))) & 1
}

fn make_file_frame(data: &[u8], pts: u64) -> EncodedFileFrame {
    let data = Bytes::copy_from_slice(data);
    let keyframe = crate::preview::annex_b_nals(&data)
        .iter()
        .any(|(kind, _)| *kind == 5);
    EncodedFileFrame {
        data,
        pts,
        keyframe,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::PreviewConfig;
    use mp4::{AvcConfig, MediaConfig, Mp4Config, Mp4Sample, Mp4Writer, TrackConfig};
    use tempfile::tempdir;

    #[test]
    fn converts_avcc_samples_to_annex_b() {
        let input = [0, 0, 0, 2, 0x67, 0x64, 0, 0, 0, 3, 0x65, 0x88, 0x84];
        let output = avcc_to_annex_b_with_length(&input, 4).unwrap();
        assert_eq!(
            output,
            [0, 0, 0, 1, 0x67, 0x64, 0, 0, 0, 1, 0x65, 0x88, 0x84]
        );
        assert!(avcc_to_annex_b_with_length(&[0, 0, 0, 8, 1], 4).is_err());
        assert_eq!(
            avcc_to_annex_b_with_length(&[2, 0x65, 0x88], 1).unwrap(),
            [0, 0, 0, 1, 0x65, 0x88]
        );
    }

    #[test]
    fn reads_h264_mp4_metadata_and_bootstraps_keyframes() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("input.mp4");
        let file = File::create(&path).unwrap();
        let mut writer = Mp4Writer::write_start(
            file,
            &Mp4Config {
                major_brand: "isom".parse().unwrap(),
                minor_version: 512,
                compatible_brands: vec!["isom".parse().unwrap(), "avc1".parse().unwrap()],
                timescale: 90_000,
            },
        )
        .unwrap();
        writer
            .add_track(&TrackConfig {
                track_type: TrackType::Video,
                timescale: 90_000,
                language: "und".into(),
                media_conf: MediaConfig::AvcConfig(AvcConfig {
                    width: 640,
                    height: 360,
                    seq_param_set: vec![0x67, 0x64, 0, 0x1f],
                    pic_param_set: vec![0x68, 0xee, 0x3c, 0x80],
                }),
            })
            .unwrap();
        writer
            .write_sample(
                1,
                &Mp4Sample {
                    start_time: 0,
                    duration: 3_600,
                    rendering_offset: 0,
                    is_sync: true,
                    bytes: Bytes::from_static(&[0, 0, 0, 3, 0x65, 1, 2]),
                },
            )
            .unwrap();
        writer.write_end().unwrap();
        let stream = read_mp4_stream(&path).unwrap();
        assert_eq!((stream.width, stream.height, stream.fps), (640, 360, 25));
        assert_eq!(stream.frames.len(), 1);
        assert!(stream.frames[0].keyframe);
        let kinds = crate::preview::annex_b_nals(&stream.frames[0].data)
            .into_iter()
            .map(|(kind, _)| kind)
            .collect::<Vec<_>>();
        assert_eq!(kinds, vec![7, 8, 5]);
    }

    #[test]
    fn assembles_annex_b_access_units_and_detects_idr() {
        let data = [
            0, 0, 0, 1, 0x67, 0x64, 0, 0, 0, 1, 0x68, 0xee, 0, 0, 0, 1, 0x65, 0x88, 0, 0, 0, 1,
            0x09, 0xf0, 0, 0, 0, 1, 0x41, 0x80,
        ];
        let frames = parse_annex_b_frames(&data, 25);
        assert_eq!(frames.len(), 2);
        assert!(frames[0].keyframe);
        assert!(!frames[1].keyframe);
        assert_eq!(frames[1].pts, 40_000);
        assert!(frames[0].data.starts_with(&[0, 0, 0, 1, 0x67]));
    }

    #[test]
    fn keeps_multiple_slices_in_one_access_unit() {
        let data = [
            0, 0, 0, 1, 0x41, 0x80, // first_mb_in_slice = 0
            0, 0, 0, 1, 0x41, 0x40, // first_mb_in_slice = 1
            0, 0, 0, 1, 0x41, 0x80, // next picture
        ];
        let frames = parse_annex_b_frames(&data, 30);
        assert_eq!(frames.len(), 2);
        assert_eq!(crate::preview::annex_b_nals(&frames[0].data).len(), 2);
    }

    #[test]
    fn encodes_aipv2_header_and_flags() {
        let encoded = encode_aipv2(&EncodedAccessUnit {
            pts: 123,
            sequence: 7,
            keyframe: true,
            discontinuity: true,
            eos: false,
            data: Bytes::from_static(&[0, 0, 0, 1, 0x67, 1]),
        });
        assert_eq!(&encoded[..4], b"AIPV");
        assert_eq!(u16::from_be_bytes(encoded[4..6].try_into().unwrap()), 2);
        assert_eq!(u16::from_be_bytes(encoded[6..8].try_into().unwrap()), 7);
        assert_eq!(u32::from_be_bytes(encoded[8..12].try_into().unwrap()), 6);
        assert_eq!(u64::from_be_bytes(encoded[12..20].try_into().unwrap()), 123);
        assert_eq!(u64::from_be_bytes(encoded[20..28].try_into().unwrap()), 7);
    }

    #[test]
    fn removes_rtsp_credentials_before_connecting_or_logging() {
        let raw = "rtsp://alice:secret@example.test/live";
        let (url, credentials) = rtsp_url_and_credentials(raw, None, None).unwrap();
        assert_eq!(url.as_str(), "rtsp://example.test/live");
        let credentials = credentials.unwrap();
        assert_eq!(credentials.username, "alice");
        assert_eq!(credentials.password, "secret");
        let error = sanitize_rtsp_error(&format!("cannot connect to {raw}"), raw);
        assert!(!error.contains("secret"));
        assert!(error.contains("***"));
    }

    #[tokio::test]
    async fn file_source_publishes_to_the_existing_preview_hub() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("sample.h264");
        std::fs::write(
            &path,
            [
                0, 0, 0, 1, 0x67, 0x64, 0, 0, 0, 1, 0x68, 0xee, 0, 0, 0, 1, 0x65, 0x88,
            ],
        )
        .unwrap();
        let (events, _) = broadcast::channel(16);
        let hub = PreviewHub::new(PreviewConfig::default(), events.clone());
        let ai = AiHub::new(1024 * 1024, events.clone());
        let manager = SourceManager::new(
            InputConfig {
                enabled: true,
                active_source: Some("file0".into()),
                file_roots: vec![temp.path().to_path_buf()],
                sources: vec![InputSourceConfig {
                    id: "file0".into(),
                    ai_sidecar: false,
                    source: InputSourceKind::File {
                        path,
                        loop_playback: false,
                        fps: 25,
                        realtime: false,
                    },
                }],
                ..InputConfig::default()
            },
            hub.clone(),
            ai,
            events,
        )
        .unwrap();
        let mut frames = hub.subscribe();
        manager.start_active().await.unwrap();
        let frame = tokio::time::timeout(Duration::from_secs(1), frames.recv())
            .await
            .unwrap()
            .unwrap();
        assert!(frame.keyframe);
        assert_eq!(frame.sequence, 0);
        assert_eq!(frame.info.codec, "h264");
        manager.shutdown().await;
    }
}
