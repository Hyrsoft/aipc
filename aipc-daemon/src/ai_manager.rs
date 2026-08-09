use crate::ai::{AiFrame, AiHub, encode_ai_frame};
use crate::ai_results::{
    AiAnnotationV1, AiBoundingBoxV1, AiFrameInfoV1, AiGenerationEventDataV1, AiInferenceInfoV1,
    AiLifecycleTracker, AiObjectV1, AiResultBus, AiResultBusStatus, AiResultInput,
    AiTrackEventDataV1, FRAME_RESULT_TYPE, GENERATION_TYPE, TRACK_ENTERED_TYPE, TRACK_EXITED_TYPE,
    TRACK_UPDATED_TYPE,
};
use crate::config::{AiDaemonConfig, AiInputConfig};
use crate::model::{ServerEvent, now_ms};
use nix::sys::signal::{Signal, kill};
use nix::unistd::Pid;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashSet, VecDeque};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream as StdUnixStream;
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::{Arc, Mutex as StdMutex, RwLock};
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::process::Command;
use tokio::sync::{Mutex, broadcast, mpsc};
use tracing::{info, warn};
use uuid::Uuid;

mod tracking;
use tracking::{Tracker, UntrackedDetection, map_point, number, render_regions};

const MAX_RESULT_BYTES: usize = 256 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AiProjectManifest {
    #[serde(default = "default_manifest_schema_version")]
    pub schema_version: u32,
    pub id: String,
    pub name: String,
    #[serde(default = "default_entry")]
    pub entry: String,
    #[serde(default = "default_algorithm")]
    pub algorithm: String,
    #[serde(default)]
    pub model: String,
    #[serde(default)]
    pub labels: String,
    #[serde(default)]
    pub files: BTreeMap<String, String>,
    #[serde(default = "default_algorithm_options")]
    pub options: Value,
    pub input: AiInputConfig,
    #[serde(default = "default_threshold")]
    pub threshold: f32,
    #[serde(default = "default_nms_threshold")]
    pub nms_threshold: f32,
    #[serde(default = "default_max_detections")]
    pub max_detections: usize,
    #[serde(default)]
    pub class_filter: Vec<i64>,
}

impl AiProjectManifest {
    fn referenced_files(&self) -> Vec<&str> {
        let mut files = Vec::new();
        for value in std::iter::once(self.model.as_str())
            .chain(std::iter::once(self.labels.as_str()))
            .chain(self.files.values().map(String::as_str))
        {
            if !value.is_empty() && !files.contains(&value) {
                files.push(value);
            }
        }
        files
    }

    pub fn validate(&self) -> anyhow::Result<()> {
        anyhow::ensure!(
            (1..=2).contains(&self.schema_version),
            "unsupported schema_version"
        );
        validate_name(&self.id)?;
        validate_name(&self.entry)?;
        anyhow::ensure!(self.entry == "main.lua", "entry must be main.lua in v1");
        if !self.model.is_empty() {
            validate_name(&self.model)?;
        }
        if !self.labels.is_empty() {
            validate_name(&self.labels)?;
        }
        anyhow::ensure!(
            matches!(
                self.algorithm.as_str(),
                "yolov5"
                    | "yolo11"
                    | "lprnet"
                    | "mlsd"
                    | "ppocr"
                    | "nanotrack"
                    | "find_blobs"
                    | "ive_filter"
                    | "ive_ncc"
                    | "npu_clock"
                    | "frame_info"
            ),
            "unsupported AI algorithm"
        );
        anyhow::ensure!(self.files.len() <= 32, "too many algorithm resource files");
        for (role, file) in &self.files {
            validate_name(role)?;
            validate_name(file)?;
        }
        anyhow::ensure!(self.options.is_object(), "options must be an object");
        if matches!(
            self.algorithm.as_str(),
            "yolov5" | "yolo11" | "lprnet" | "mlsd" | "ive_ncc"
        ) {
            anyhow::ensure!(!self.model.is_empty(), "algorithm requires model");
        }
        if self.algorithm == "ppocr" {
            anyhow::ensure!(!self.model.is_empty(), "ppocr requires detector model");
            anyhow::ensure!(
                self.files.contains_key("recognizer"),
                "ppocr requires files.recognizer"
            );
            anyhow::ensure!(
                self.files.contains_key("dictionary"),
                "ppocr requires files.dictionary"
            );
        }
        if self.algorithm == "nanotrack" {
            anyhow::ensure!(!self.model.is_empty(), "nanotrack requires template model");
            anyhow::ensure!(
                self.files.contains_key("search"),
                "nanotrack requires files.search"
            );
            anyhow::ensure!(
                self.files.contains_key("head"),
                "nanotrack requires files.head"
            );
        }
        anyhow::ensure!(
            (0.0..=1.0).contains(&self.threshold) && (0.0..=1.0).contains(&self.nms_threshold),
            "thresholds must be in [0, 1]"
        );
        anyhow::ensure!(
            (1..=256).contains(&self.max_detections),
            "max_detections must be in [1, 256]"
        );
        anyhow::ensure!(
            self.class_filter.len() <= 256
                && self
                    .class_filter
                    .iter()
                    .all(|value| (0..=10000).contains(value)),
            "class_filter contains an invalid class id"
        );
        let mut worker = crate::config::WorkerConfig::default();
        worker.ai_input = self.input.clone();
        let errors = worker.validate();
        anyhow::ensure!(errors.is_empty(), "invalid AI input: {}", errors.join("; "));
        Ok(())
    }

    fn validate_runtime_guard(&self) -> anyhow::Result<()> {
        if let Some(reason) = self.options.get("runtime_guard").and_then(Value::as_str) {
            anyhow::ensure!(
                self.options
                    .get("runtime_guard_ack")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                "runtime guard: {reason}; set options.runtime_guard_ack=true only after verifying the board RKNN runtime"
            );
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AiProjectDocument {
    pub manifest: AiProjectManifest,
    pub script: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct AiProjectSummary {
    pub id: String,
    pub name: String,
    pub algorithm: String,
    pub model: String,
    pub input: AiInputConfig,
    pub active: bool,
    pub last_good: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct AiModelInfo {
    pub name: String,
    pub bytes: u64,
    pub sha256: String,
    pub active: bool,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(rename_all = "snake_case")]
pub enum OsdMode {
    Off,
    #[default]
    Metadata,
    EmbeddedRgn,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AiDetection {
    pub track_id: u64,
    pub class_id: i64,
    pub label: String,
    pub confidence: f64,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AiMetadata {
    pub version: u32,
    pub generation: String,
    pub sequence: u64,
    pub pts: u64,
    pub main_width: u32,
    pub main_height: u32,
    pub inference_us: u64,
    pub detections: Vec<AiDetection>,
    pub annotations: Vec<AiAnnotationV1>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AiProcessState {
    Stopped,
    Starting,
    Running,
    RollingBack,
    Failed,
}

#[derive(Debug, Clone, Serialize)]
pub struct AiStatus {
    pub enabled: bool,
    pub state: AiProcessState,
    pub pid: Option<u32>,
    pub generation: Option<String>,
    pub active_project: Option<String>,
    pub last_good_project: Option<String>,
    pub worker_ready: bool,
    pub first_inference: bool,
    pub input: crate::ai::AiInputStatus,
    pub results: u64,
    pub inference_fps: f64,
    pub average_inference_ms: f64,
    pub last_result_at_ms: Option<u64>,
    pub last_error: Option<String>,
    pub osd_mode: OsdMode,
    pub rgn_capability: Option<Value>,
    pub result_bus: AiResultBusStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
struct PersistedAiState {
    schema_version: u32,
    active_project: Option<String>,
    last_good_project: Option<String>,
    #[serde(default)]
    osd_mode: OsdMode,
}

struct Runtime {
    pid: u32,
    generation: String,
}

#[derive(Clone)]
struct InferenceDescriptor {
    project: String,
    algorithm: String,
    model: String,
}

#[derive(Clone)]
struct TimedMetadata {
    metadata: Arc<AiMetadata>,
    received_at: Instant,
}

struct RenderedRegions {
    generation: String,
    main_width: u32,
    main_height: u32,
    regions: Vec<AiDetection>,
}

struct Inner {
    config: AiDaemonConfig,
    root: PathBuf,
    projects: PathBuf,
    models: PathBuf,
    deployments: PathBuf,
    hub: AiHub,
    events: broadcast::Sender<ServerEvent>,
    state: Mutex<PersistedAiState>,
    status: RwLock<AiStatus>,
    runtime: Mutex<Option<Runtime>>,
    transition: Mutex<()>,
    metadata: broadcast::Sender<Arc<AiMetadata>>,
    last_metadata: RwLock<Option<Arc<AiMetadata>>>,
    metadata_history: RwLock<VecDeque<TimedMetadata>>,
    tracker: Mutex<Tracker>,
    result_bus: AiResultBus,
    lifecycle: StdMutex<AiLifecycleTracker>,
    active_inference: RwLock<Option<InferenceDescriptor>>,
    last_media_generation: Mutex<Option<String>>,
    recovery_times: Mutex<VecDeque<u64>>,
    recovery_tx: mpsc::Sender<(String, String)>,
    intentional_stops: Mutex<HashSet<String>>,
}

#[derive(Clone)]
pub struct AiManager {
    inner: Arc<Inner>,
}

impl AiManager {
    pub async fn new(
        config: AiDaemonConfig,
        data_dir: &Path,
        hub: AiHub,
        events: broadcast::Sender<ServerEvent>,
    ) -> anyhow::Result<Self> {
        let root = data_dir.join("ai");
        let projects = root.join("projects");
        let models = root.join("models");
        let deployments = root.join("deployments");
        for directory in [&root, &projects, &models, &deployments] {
            tokio::fs::create_dir_all(directory).await?;
        }
        let state_path = root.join("state.json");
        let state = match tokio::fs::read(&state_path).await {
            Ok(data) => serde_json::from_slice(&data)?,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => PersistedAiState {
                schema_version: 1,
                osd_mode: OsdMode::Metadata,
                ..PersistedAiState::default()
            },
            Err(error) => return Err(error.into()),
        };
        let (metadata, _) = broadcast::channel(32);
        let (recovery_tx, recovery_rx) = mpsc::channel(4);
        let result_bus = AiResultBus::new(config.source_id.clone(), config.result_replay_capacity);
        let lifecycle = AiLifecycleTracker::new(
            config.track_confirmations,
            config.track_lost_timeout_ms,
            config.track_update_interval_ms,
        );
        let tracker_retention_us = config.track_lost_timeout_ms.saturating_mul(1_000);
        let status = AiStatus {
            enabled: config.enabled,
            state: AiProcessState::Stopped,
            pid: None,
            generation: None,
            active_project: state.active_project.clone(),
            last_good_project: state.last_good_project.clone(),
            worker_ready: false,
            first_inference: false,
            input: hub.status(),
            results: 0,
            inference_fps: 0.0,
            average_inference_ms: 0.0,
            last_result_at_ms: None,
            last_error: None,
            osd_mode: state.osd_mode,
            rgn_capability: None,
            result_bus: result_bus.status(),
        };
        let manager = Self {
            inner: Arc::new(Inner {
                config,
                root,
                projects,
                models,
                deployments,
                hub,
                events,
                state: Mutex::new(state),
                status: RwLock::new(status),
                runtime: Mutex::new(None),
                transition: Mutex::new(()),
                metadata,
                last_metadata: RwLock::new(None),
                metadata_history: RwLock::new(VecDeque::with_capacity(2)),
                tracker: Mutex::new(Tracker::new(tracker_retention_us)),
                result_bus,
                lifecycle: StdMutex::new(lifecycle),
                active_inference: RwLock::new(None),
                last_media_generation: Mutex::new(None),
                recovery_times: Mutex::new(VecDeque::new()),
                recovery_tx,
                intentional_stops: Mutex::new(HashSet::new()),
            }),
        };
        manager.spawn_recovery_loop(recovery_rx);
        manager.spawn_media_reconcile_loop();
        manager.spawn_embedded_osd_loop();
        manager.spawn_result_lifecycle_loop();
        Ok(manager)
    }

    pub fn start_persisted(&self) {
        if !self.inner.config.enabled {
            return;
        }
        let manager = self.clone();
        tokio::spawn(async move {
            let (project, osd_mode) = {
                let state = manager.inner.state.lock().await;
                (
                    state
                        .active_project
                        .clone()
                        .or_else(|| state.last_good_project.clone()),
                    state.osd_mode,
                )
            };
            for _ in 0..150 {
                if manager.inner.hub.status().control_available {
                    if let Err(error) = manager.set_osd_mode(osd_mode).await {
                        manager.set_error(format!("restore persisted OSD mode: {error}"));
                    }
                    let Some(project) = project else { return };
                    let _transition = manager.inner.transition.lock().await;
                    if let Err(error) = manager
                        .validate_project(&project)
                        .await
                        .and_then(|_| Ok(()))
                    {
                        manager.set_error(format!("validate persisted AI project: {error}"));
                        return;
                    }
                    if let Err(error) = manager.activate(&project, true).await {
                        manager.set_error(format!("start persisted AI project: {error}"));
                    }
                    return;
                }
                tokio::time::sleep(Duration::from_millis(100)).await;
            }
            manager.set_error("media control did not become ready for persisted AI project".into());
        });
    }

    pub fn status(&self) -> AiStatus {
        let mut status = self.inner.status.read().unwrap().clone();
        status.input = self.inner.hub.status();
        status.result_bus = self.inner.result_bus.status();
        status
    }

    pub async fn shutdown(&self) {
        let _transition = self.inner.transition.lock().await;
        self.stop_runtime().await;
        let _ = self
            .inner
            .hub
            .media_request("set_osd_mode", json!({"mode": "off"}))
            .await;
        self.inner.status.write().unwrap().state = AiProcessState::Stopped;
    }

    pub async fn stop_for_maintenance(&self) -> Option<String> {
        let _transition = self.inner.transition.lock().await;
        let project = {
            let state = self.inner.state.lock().await;
            state
                .active_project
                .clone()
                .or_else(|| state.last_good_project.clone())
        };
        self.stop_runtime().await;
        self.inner.status.write().unwrap().state = AiProcessState::Stopped;
        project
    }

    pub async fn start_for_maintenance(&self, project: Option<String>) -> anyhow::Result<()> {
        let Some(project) = project else {
            return Ok(());
        };
        let _transition = self.inner.transition.lock().await;
        anyhow::ensure!(self.inner.config.enabled, "AI is disabled");
        self.validate_project(&project).await?;
        self.activate(&project, true).await
    }

    pub async fn restart_for_maintenance(&self) -> anyhow::Result<()> {
        let project = {
            let state = self.inner.state.lock().await;
            state
                .active_project
                .clone()
                .or_else(|| state.last_good_project.clone())
        };
        let Some(project) = project else {
            return Ok(());
        };
        let _transition = self.inner.transition.lock().await;
        anyhow::ensure!(self.inner.config.enabled, "AI is disabled");
        self.validate_project(&project).await?;
        self.activate(&project, true).await
    }

    pub fn subscribe_metadata(&self) -> broadcast::Receiver<Arc<AiMetadata>> {
        self.inner.metadata.subscribe()
    }

    pub fn latest_result(&self) -> Option<Arc<crate::ai_results::AiCloudEvent>> {
        self.inner.result_bus.latest()
    }

    pub fn subscribe_results(
        &self,
        cursor: Option<&str>,
    ) -> crate::ai_results::AiResultSubscription {
        self.inner.result_bus.subscribe_from(cursor)
    }

    pub fn replay_results_after(
        &self,
        sequence: u64,
    ) -> (VecDeque<Arc<crate::ai_results::AiCloudEvent>>, u64) {
        self.inner.result_bus.replay_after_sequence(sequence)
    }

    pub fn record_result_lag(&self, skipped: u64) {
        self.inner.result_bus.record_lagged(skipped);
    }

    pub fn result_schema() -> &'static str {
        AiResultBus::schema()
    }

    pub async fn list_projects(&self) -> anyhow::Result<Vec<AiProjectSummary>> {
        let state = self.inner.state.lock().await.clone();
        let mut reader = tokio::fs::read_dir(&self.inner.projects).await?;
        let mut projects = Vec::new();
        while let Some(entry) = reader.next_entry().await? {
            if !entry.file_type().await?.is_dir() {
                continue;
            }
            let entry_name = entry.file_name();
            let id = entry_name.to_string_lossy();
            // A watchdog reset can interrupt an atomic PUT after the staging
            // directory has been synced but before it is renamed. Hidden
            // staging/backup directories are recovery artifacts, not projects.
            if id.starts_with('.') {
                continue;
            }
            if let Ok(document) = self.get_project(&id).await {
                projects.push(AiProjectSummary {
                    id: document.manifest.id.clone(),
                    name: document.manifest.name.clone(),
                    algorithm: document.manifest.algorithm.clone(),
                    model: document.manifest.model.clone(),
                    input: document.manifest.input.clone(),
                    active: state.active_project.as_deref() == Some(&document.manifest.id),
                    last_good: state.last_good_project.as_deref() == Some(&document.manifest.id),
                });
            }
        }
        projects.sort_by(|left, right| left.id.cmp(&right.id));
        Ok(projects)
    }

    pub async fn get_project(&self, id: &str) -> anyhow::Result<AiProjectDocument> {
        validate_name(id)?;
        let directory = self.inner.projects.join(id);
        let manifest =
            serde_json::from_slice(&tokio::fs::read(directory.join("manifest.json")).await?)?;
        let script = tokio::fs::read_to_string(directory.join("main.lua")).await?;
        Ok(AiProjectDocument { manifest, script })
    }

    pub async fn put_project(
        &self,
        id: &str,
        document: AiProjectDocument,
    ) -> anyhow::Result<AiProjectDocument> {
        validate_name(id)?;
        anyhow::ensure!(document.manifest.id == id, "manifest id must match URL id");
        document.manifest.validate()?;
        anyhow::ensure!(
            document.script.len() <= 512 * 1024,
            "Lua script is too large"
        );
        let target = self.inner.projects.join(id);
        let staging = self
            .inner
            .projects
            .join(format!(".{id}.{}.part", Uuid::new_v4()));
        tokio::fs::create_dir_all(&staging).await?;
        write_synced(
            &staging.join("manifest.json"),
            &serde_json::to_vec_pretty(&document.manifest)?,
        )
        .await?;
        write_synced(&staging.join("main.lua"), document.script.as_bytes()).await?;
        let backup = self
            .inner
            .projects
            .join(format!(".{id}.{}.previous", Uuid::new_v4()));
        if tokio::fs::metadata(&target).await.is_ok() {
            tokio::fs::rename(&target, &backup).await?;
        }
        if let Err(error) = tokio::fs::rename(&staging, &target).await {
            if tokio::fs::metadata(&backup).await.is_ok() {
                let _ = tokio::fs::rename(&backup, &target).await;
            }
            return Err(error.into());
        }
        if tokio::fs::metadata(&backup).await.is_ok() {
            tokio::fs::remove_dir_all(&backup).await?;
        }
        Ok(document)
    }

    pub async fn delete_project(&self, id: &str) -> anyhow::Result<()> {
        validate_name(id)?;
        let state = self.inner.state.lock().await;
        anyhow::ensure!(
            state.active_project.as_deref() != Some(id)
                && state.last_good_project.as_deref() != Some(id),
            "active or last-good project cannot be deleted"
        );
        drop(state);
        tokio::fs::remove_dir_all(self.inner.projects.join(id)).await?;
        Ok(())
    }

    pub async fn validate_project(&self, id: &str) -> anyhow::Result<Value> {
        let document = self.get_project(id).await?;
        document.manifest.validate()?;
        for file in document.manifest.referenced_files() {
            anyhow::ensure!(
                tokio::fs::metadata(self.inner.models.join(file))
                    .await
                    .is_ok(),
                "AI resource {file} does not exist"
            );
        }
        let output = Command::new(&self.inner.config.worker_path)
            .arg("--project-dir")
            .arg(self.inner.projects.join(id))
            .arg("--models-dir")
            .arg(&self.inner.models)
            .arg("--validate-only")
            .arg("--mock")
            .output()
            .await?;
        anyhow::ensure!(
            output.status.success(),
            "ai_worker validation failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        Ok(json!({"valid": true, "project": id}))
    }

    pub async fn deploy(&self, id: &str) -> anyhow::Result<AiStatus> {
        let _transition = self.inner.transition.lock().await;
        anyhow::ensure!(self.inner.config.enabled, "AI is disabled");
        self.validate_project(id).await?;
        self.get_project(id)
            .await?
            .manifest
            .validate_runtime_guard()?;
        let previous = self.inner.state.lock().await.last_good_project.clone();
        match self.activate(id, false).await {
            Ok(()) => {
                let mut state = self.inner.state.lock().await;
                state.active_project = Some(id.to_owned());
                state.last_good_project = Some(id.to_owned());
                self.persist_state(&state).await?;
                let mut status = self.inner.status.write().unwrap();
                status.active_project = Some(id.to_owned());
                status.last_good_project = Some(id.to_owned());
                drop(status);
                Ok(self.status())
            }
            Err(candidate_error) => {
                self.set_error(format!("candidate {id} failed: {candidate_error}"));
                if let Some(previous) = previous.filter(|value| value != id) {
                    self.inner.status.write().unwrap().state = AiProcessState::RollingBack;
                    if let Err(rollback_error) = self.activate(&previous, true).await {
                        self.set_error(format!(
                            "candidate failed: {candidate_error}; rollback failed: {rollback_error}"
                        ));
                    }
                }
                Err(candidate_error)
            }
        }
    }

    async fn activate(&self, id: &str, rollback: bool) -> anyhow::Result<()> {
        self.stop_runtime().await;
        let document = self.get_project(id).await?;
        let generation = Uuid::new_v4().to_string();
        let snapshot = self.inner.deployments.join(&generation);
        tokio::fs::create_dir_all(&snapshot).await?;
        tokio::fs::write(
            snapshot.join("manifest.json"),
            serde_json::to_vec_pretty(&document.manifest)?,
        )
        .await?;
        tokio::fs::write(snapshot.join("main.lua"), document.script).await?;
        self.inner
            .hub
            .configure_input(document.manifest.input.clone())
            .await?;
        *self.inner.active_inference.write().unwrap() = Some(InferenceDescriptor {
            project: document.manifest.id.clone(),
            algorithm: document.manifest.algorithm.clone(),
            model: document.manifest.model.clone(),
        });
        *self.inner.last_media_generation.lock().await = self.inner.hub.status().generation.clone();
        {
            let mut status = self.inner.status.write().unwrap();
            status.state = if rollback {
                AiProcessState::RollingBack
            } else {
                AiProcessState::Starting
            };
            status.generation = Some(generation.clone());
            status.pid = None;
            status.worker_ready = false;
            status.first_inference = false;
            status.results = 0;
            status.inference_fps = 0.0;
            status.average_inference_ms = 0.0;
            status.last_result_at_ms = None;
            status.last_error = None;
        }
        let (runtime, mut messages) = self.spawn_worker(&snapshot, &generation).await?;
        self.inner.status.write().unwrap().pid = Some(runtime.pid);
        *self.inner.runtime.lock().await = Some(runtime);
        let deadline = tokio::time::Instant::now()
            + Duration::from_millis(self.inner.config.startup_timeout_ms);
        let mut ready = false;
        let mut inference = false;
        while !(ready && inference) {
            let message = tokio::time::timeout_at(deadline, messages.recv())
                .await
                .map_err(|_| anyhow::anyhow!("AI startup timed out"))?
                .ok_or_else(|| anyhow::anyhow!("AI worker message channel closed"))?;
            match message {
                ProcessMessage::Ready => {
                    ready = true;
                    self.inner.status.write().unwrap().worker_ready = true;
                }
                ProcessMessage::Result(value, frame) => {
                    inference = true;
                    self.handle_result(&generation, value, &frame).await?;
                    self.inner.status.write().unwrap().first_inference = true;
                }
                ProcessMessage::Error(error) => self.set_error(error),
                ProcessMessage::Log(line) => self.record_log(line),
                ProcessMessage::Exited(code, signal) => {
                    anyhow::bail!(
                        "AI worker exited during startup: code={code:?} signal={signal:?}"
                    )
                }
            }
        }
        self.inner.status.write().unwrap().state = AiProcessState::Running;
        let manager = self.clone();
        let active_generation = generation.clone();
        tokio::spawn(async move {
            while let Some(message) = messages.recv().await {
                match message {
                    ProcessMessage::Result(value, frame) => {
                        if let Err(error) = manager
                            .handle_result(&active_generation, value, &frame)
                            .await
                        {
                            manager.set_error(error.to_string());
                        }
                    }
                    ProcessMessage::Error(error) => manager.set_error(error),
                    ProcessMessage::Log(line) => manager.record_log(line),
                    ProcessMessage::Exited(code, signal) => {
                        if manager
                            .inner
                            .intentional_stops
                            .lock()
                            .await
                            .remove(&active_generation)
                        {
                            break;
                        }
                        manager.finish_result_generation("worker_exited");
                        let current = manager.inner.status.read().unwrap().generation.clone();
                        if current.as_deref() == Some(&active_generation) {
                            let mut status = manager.inner.status.write().unwrap();
                            status.state = AiProcessState::Failed;
                            status.pid = None;
                            status.last_error =
                                Some(format!("AI worker exited: code={code:?} signal={signal:?}"));
                        }
                        manager.schedule_recovery(&active_generation).await;
                        break;
                    }
                    ProcessMessage::Ready => {}
                }
            }
        });
        let _ = self.inner.events.send(ServerEvent::new(
            "ai_deployed",
            json!({"project": id, "generation": generation, "rollback": rollback}),
        ));
        Ok(())
    }

    async fn spawn_worker(
        &self,
        project_dir: &Path,
        generation: &str,
    ) -> anyhow::Result<(Runtime, mpsc::Receiver<ProcessMessage>)> {
        let (frame_parent, frame_child) = StdUnixStream::pair()?;
        let (result_parent, result_child) = StdUnixStream::pair()?;
        frame_parent.set_nonblocking(true)?;
        result_parent.set_nonblocking(true)?;
        let frame_fd = frame_child.as_raw_fd();
        let result_fd = result_child.as_raw_fd();
        let mut command = Command::new(&self.inner.config.worker_path);
        command
            .arg("--project-dir")
            .arg(project_dir)
            .arg("--models-dir")
            .arg(&self.inner.models)
            .arg("--input-fd")
            .arg("3")
            .arg("--output-fd")
            .arg("4")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .kill_on_drop(false);
        unsafe {
            command.pre_exec(move || {
                if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) != 0 {
                    return Err(std::io::Error::last_os_error());
                }
                let frame_temp = libc::fcntl(frame_fd, libc::F_DUPFD_CLOEXEC, 10);
                let result_temp = libc::fcntl(result_fd, libc::F_DUPFD_CLOEXEC, 11);
                if frame_temp < 0
                    || result_temp < 0
                    || libc::dup2(frame_temp, 3) < 0
                    || libc::dup2(result_temp, 4) < 0
                {
                    return Err(std::io::Error::last_os_error());
                }
                libc::close(frame_temp);
                libc::close(result_temp);
                Ok(())
            });
        }
        let mut child = command.spawn()?;
        drop(frame_child);
        drop(result_child);
        let pid = child
            .id()
            .ok_or_else(|| anyhow::anyhow!("AI worker has no PID"))?;
        let stderr = child.stderr.take().unwrap();
        let frame_stream = tokio::net::UnixStream::from_std(frame_parent)?;
        let result_stream = tokio::net::UnixStream::from_std(result_parent)?;
        let frames = Arc::new(Mutex::new(VecDeque::<Arc<AiFrame>>::with_capacity(8)));
        let (messages_tx, messages_rx) = mpsc::channel(64);
        spawn_frame_forwarder(
            frame_stream,
            self.inner.hub.subscribe_frames(),
            frames.clone(),
            messages_tx.clone(),
        );
        spawn_result_reader(result_stream, frames, messages_tx.clone());
        spawn_stderr_reader(stderr, messages_tx.clone());
        tokio::spawn(async move {
            match child.wait().await {
                Ok(status) => {
                    let _ = messages_tx
                        .send(ProcessMessage::Exited(status.code(), status.signal()))
                        .await;
                }
                Err(error) => {
                    let _ = messages_tx
                        .send(ProcessMessage::Error(format!("wait AI worker: {error}")))
                        .await;
                }
            }
        });
        Ok((
            Runtime {
                pid,
                generation: generation.to_owned(),
            },
            messages_rx,
        ))
    }

    async fn stop_runtime(&self) {
        self.finish_result_generation("worker_stopped");
        if let Some(runtime) = self.inner.runtime.lock().await.take() {
            self.inner
                .intentional_stops
                .lock()
                .await
                .insert(runtime.generation.clone());
            let _ = kill(Pid::from_raw(runtime.pid as i32), Signal::SIGTERM);
            info!(pid = runtime.pid, generation = %runtime.generation, "stopping AI worker");
            tokio::time::sleep(Duration::from_millis(250)).await;
            if kill(Pid::from_raw(runtime.pid as i32), None).is_ok() {
                let _ = kill(Pid::from_raw(runtime.pid as i32), Signal::SIGKILL);
            }
        }
        {
            let mut status = self.inner.status.write().unwrap();
            status.pid = None;
            status.worker_ready = false;
            status.first_inference = false;
        }
        self.inner.metadata_history.write().unwrap().clear();
        *self.inner.last_metadata.write().unwrap() = None;
        *self.inner.tracker.lock().await = Tracker::new(
            self.inner
                .config
                .track_lost_timeout_ms
                .saturating_mul(1_000),
        );
        *self.inner.active_inference.write().unwrap() = None;
        self.clear_embedded_regions().await;
    }

    async fn schedule_recovery(&self, failed_generation: &str) {
        let project = self.inner.state.lock().await.last_good_project.clone();
        let Some(project) = project else { return };
        {
            let now = now_ms();
            let mut recoveries = self.inner.recovery_times.lock().await;
            while recoveries
                .front()
                .is_some_and(|value| now.saturating_sub(*value) > 60_000)
            {
                recoveries.pop_front();
            }
            if recoveries.len() >= 5 {
                self.set_error("AI restart rate limit reached (5 per minute)".into());
                return;
            }
            recoveries.push_back(now);
        }
        let _ = self
            .inner
            .recovery_tx
            .send((failed_generation.to_owned(), project))
            .await;
    }

    fn spawn_recovery_loop(&self, mut receiver: mpsc::Receiver<(String, String)>) {
        let manager = self.clone();
        tokio::spawn(async move {
            while let Some((failed_generation, project)) = receiver.recv().await {
                tokio::time::sleep(Duration::from_secs(1)).await;
                if manager.inner.status.read().unwrap().generation.as_deref()
                    != Some(&failed_generation)
                {
                    continue;
                }
                let _transition = manager.inner.transition.lock().await;
                if let Err(error) = manager.activate(&project, true).await {
                    manager.set_error(format!("automatic last-good restart failed: {error}"));
                }
            }
        });
    }

    fn spawn_media_reconcile_loop(&self) {
        let manager = self.clone();
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(Duration::from_millis(500));
            loop {
                tick.tick().await;
                let input = manager.inner.hub.status();
                let Some(media_generation) = input.generation.clone() else {
                    continue;
                };
                if !input.control_available
                    || manager.inner.runtime.lock().await.is_none()
                    || manager.inner.last_media_generation.lock().await.as_deref()
                        == Some(media_generation.as_str())
                {
                    continue;
                }

                let _transition = manager.inner.transition.lock().await;
                let input = manager.inner.hub.status();
                if !input.control_available
                    || input.generation.as_deref() != Some(media_generation.as_str())
                    || manager.inner.runtime.lock().await.is_none()
                    || manager.inner.last_media_generation.lock().await.as_deref()
                        == Some(media_generation.as_str())
                {
                    continue;
                }
                let (project, osd_mode) = {
                    let state = manager.inner.state.lock().await;
                    (state.active_project.clone(), state.osd_mode)
                };
                let Some(project) = project else { continue };
                let document = match manager.get_project(&project).await {
                    Ok(document) => document,
                    Err(error) => {
                        manager.set_error(format!(
                            "reload active AI project after media restart: {error}"
                        ));
                        continue;
                    }
                };
                if let Err(error) = manager
                    .inner
                    .hub
                    .configure_input(document.manifest.input)
                    .await
                {
                    manager.set_error(format!("restore AI input after media restart: {error}"));
                    continue;
                }
                *manager.inner.last_media_generation.lock().await = Some(media_generation.clone());
                if let Err(error) = manager.apply_osd_mode(osd_mode, false).await {
                    manager.set_error(format!("restore OSD mode after media restart: {error}"));
                }
                let _ = manager.inner.events.send(ServerEvent::new(
                    "ai_media_reconciled",
                    json!({"project": project, "media_generation": media_generation}),
                ));
            }
        });
    }

    fn spawn_embedded_osd_loop(&self) {
        let manager = self.clone();
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(Duration::from_millis(66));
            let mut empty_sent = false;
            let mut last_error = None;
            loop {
                tick.tick().await;
                if manager.inner.status.read().unwrap().osd_mode != OsdMode::EmbeddedRgn {
                    empty_sent = false;
                    last_error = None;
                    continue;
                }
                let rendered = {
                    let history = manager.inner.metadata_history.read().unwrap();
                    render_regions(
                        &history,
                        Instant::now(),
                        Duration::from_millis(manager.inner.config.result_ttl_ms),
                    )
                };
                let Some(rendered) = rendered else {
                    if empty_sent {
                        continue;
                    }
                    if manager.clear_embedded_regions().await {
                        empty_sent = true;
                    }
                    continue;
                };
                if rendered.regions.is_empty() && empty_sent {
                    continue;
                }
                empty_sent = rendered.regions.is_empty();
                let regions = rendered
                    .regions
                    .iter()
                    .map(|item| {
                        json!({
                            "x": (item.x * rendered.main_width as f64).round() as i64,
                            "y": (item.y * rendered.main_height as f64).round() as i64,
                            "width": (item.width * rendered.main_width as f64).round() as i64,
                            "height": (item.height * rendered.main_height as f64).round() as i64,
                        })
                    })
                    .collect::<Vec<_>>();
                let result = manager
                    .inner
                    .hub
                    .media_request(
                        "update_regions",
                        json!({
                            "generation": rendered.generation,
                            "timestamp": now_ms(),
                            "ttl_ms": manager.inner.config.result_ttl_ms.clamp(250, 2000),
                            "main_width": rendered.main_width,
                            "main_height": rendered.main_height,
                            "regions": regions,
                        }),
                    )
                    .await;
                match result {
                    Ok(_) => last_error = None,
                    Err(error) => {
                        let error = format!("update embedded RGN: {error}");
                        if last_error.as_deref() != Some(error.as_str()) {
                            manager.set_error(error.clone());
                            last_error = Some(error);
                        }
                    }
                }
            }
        });
    }

    fn spawn_result_lifecycle_loop(&self) {
        let manager = self.clone();
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(Duration::from_millis(100));
            loop {
                tick.tick().await;
                let exited = manager.inner.lifecycle.lock().unwrap().expire(now_ms());
                for event in exited {
                    manager.publish_track_event(TRACK_EXITED_TYPE, event);
                }
            }
        });
    }

    async fn clear_embedded_regions(&self) -> bool {
        if self.inner.status.read().unwrap().osd_mode != OsdMode::EmbeddedRgn {
            return true;
        }
        let generation = self.inner.status.read().unwrap().generation.clone();
        self.inner
            .hub
            .media_request(
                "update_regions",
                json!({
                    "generation": generation,
                    "timestamp": now_ms(),
                    "ttl_ms": 250,
                    "regions": [],
                }),
            )
            .await
            .is_ok()
    }

    async fn handle_result(
        &self,
        generation: &str,
        value: Value,
        frame: &AiFrame,
    ) -> anyhow::Result<()> {
        let sequence = value
            .get("sequence")
            .and_then(Value::as_u64)
            .ok_or_else(|| anyhow::anyhow!("AI result has no sequence"))?;
        anyhow::ensure!(
            sequence == frame.sequence,
            "AI result/frame sequence mismatch"
        );
        let inference_us = value
            .get("inference_us")
            .and_then(Value::as_u64)
            .unwrap_or_default();
        let raw = value
            .get("detections")
            .and_then(Value::as_array)
            .ok_or_else(|| anyhow::anyhow!("AI detections must be an array"))?;
        let mut boxes = Vec::new();
        let mut annotations = Vec::new();
        for detection in raw.iter().take(256) {
            let x1 = number(detection, "x1")?;
            let y1 = number(detection, "y1")?;
            let x2 = number(detection, "x2")?;
            let y2 = number(detection, "y2")?;
            let (x1, y1) = map_point(frame, x1, y1);
            let (x2, y2) = map_point(frame, x2, y2);
            let label: String = detection
                .get("label")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .chars()
                .take(128)
                .collect();
            let confidence = detection
                .get("confidence")
                .and_then(Value::as_f64)
                .unwrap_or_default();
            let mut data = detection.as_object().cloned().unwrap_or_default();
            let kind = data
                .remove("kind")
                .and_then(|value| value.as_str().map(str::to_owned))
                .unwrap_or_else(|| "object".into());
            for key in ["x1", "y1", "x2", "y2", "confidence", "class_id", "label"] {
                data.remove(key);
            }
            annotations.push(AiAnnotationV1 {
                kind,
                label: label.clone(),
                confidence,
                bbox: AiBoundingBoxV1 {
                    x: x1.min(x2),
                    y: y1.min(y2),
                    width: (x2 - x1).abs(),
                    height: (y2 - y1).abs(),
                },
                data: Value::Object(data),
            });
            boxes.push(UntrackedDetection {
                class_id: detection
                    .get("class_id")
                    .and_then(Value::as_i64)
                    .unwrap_or_default(),
                label,
                confidence,
                x: x1.min(x2),
                y: y1.min(y2),
                width: (x2 - x1).abs(),
                height: (y2 - y1).abs(),
            });
        }
        let detections =
            self.inner
                .tracker
                .lock()
                .await
                .update(boxes, frame.pts, &frame.generation);
        let descriptor = self
            .inner
            .active_inference
            .read()
            .unwrap()
            .clone()
            .unwrap_or_else(|| InferenceDescriptor {
                project: "unknown".into(),
                algorithm: "unknown".into(),
                model: "unknown".into(),
            });
        let published_at_ms = now_ms();
        self.publish_standard_result(AiResultInput {
            source_id: frame.source_id.clone(),
            media_generation: frame.generation.clone(),
            ai_generation: generation.to_owned(),
            sequence,
            pts_us: frame.pts,
            published_at_ms,
            frame: AiFrameInfoV1 {
                width: frame.main_width,
                height: frame.main_height,
                coordinate_space: "main_normalized_top_left".into(),
            },
            inference: AiInferenceInfoV1 {
                project: descriptor.project,
                algorithm: descriptor.algorithm,
                model: descriptor.model,
                duration_us: inference_us,
            },
            objects: detections
                .iter()
                .map(|detection| AiObjectV1 {
                    track_id: detection.track_id,
                    class_id: detection.class_id,
                    label: detection.label.clone(),
                    confidence: detection.confidence,
                    bbox: AiBoundingBoxV1 {
                        x: detection.x,
                        y: detection.y,
                        width: detection.width,
                        height: detection.height,
                    },
                })
                .collect(),
            annotations: annotations.clone(),
        });
        let metadata = Arc::new(AiMetadata {
            version: 1,
            generation: generation.to_owned(),
            sequence,
            pts: frame.pts,
            main_width: frame.main_width,
            main_height: frame.main_height,
            inference_us,
            detections,
            annotations,
        });
        *self.inner.last_metadata.write().unwrap() = Some(metadata.clone());
        {
            let mut history = self.inner.metadata_history.write().unwrap();
            if history
                .back()
                .is_some_and(|item| item.metadata.generation != metadata.generation)
            {
                history.clear();
            }
            if history.len() == 2 {
                history.pop_front();
            }
            history.push_back(TimedMetadata {
                metadata: metadata.clone(),
                received_at: Instant::now(),
            });
        }
        let _ = self.inner.metadata.send(metadata.clone());
        let observed_at = published_at_ms;
        let mut status = self.inner.status.write().unwrap();
        if let Some(previous) = status.last_result_at_ms {
            let elapsed_ms = observed_at.saturating_sub(previous);
            if elapsed_ms > 0 {
                let instantaneous_fps = 1000.0 / elapsed_ms as f64;
                status.inference_fps = if status.inference_fps == 0.0 {
                    instantaneous_fps
                } else {
                    status.inference_fps * 0.8 + instantaneous_fps * 0.2
                };
            }
        }
        status.results += 1;
        status.last_result_at_ms = Some(observed_at);
        let count = status.results as f64;
        status.average_inference_ms =
            ((status.average_inference_ms * (count - 1.0)) + inference_us as f64 / 1000.0) / count;
        let _ = self.inner.events.send(ServerEvent::new(
            "ai_result",
            serde_json::to_value(&*metadata)?,
        ));
        Ok(())
    }

    fn publish_standard_result(&self, input: AiResultInput) {
        self.inner.result_bus.set_source_id(&input.source_id);
        let batch = self.inner.lifecycle.lock().unwrap().observe(&input);
        for event in batch.exited {
            self.publish_track_event(TRACK_EXITED_TYPE, event);
        }
        if let Some(event) = batch.generation {
            self.publish_generation_event(event);
        }
        self.inner.result_bus.publish(
            FRAME_RESULT_TYPE,
            format!("frame/{}/{}", input.media_generation, input.sequence),
            &input.data(),
            true,
        );
        for event in batch.entered {
            self.publish_track_event(TRACK_ENTERED_TYPE, event);
        }
        for event in batch.updated {
            self.publish_track_event(TRACK_UPDATED_TYPE, event);
        }
    }

    #[cfg(test)]
    pub(crate) fn publish_test_result(&self, input: AiResultInput) {
        self.publish_standard_result(input);
    }

    fn publish_track_event(&self, event_type: &str, event: AiTrackEventDataV1) {
        self.inner.result_bus.publish(
            event_type,
            format!("track/{}/{}", event.ai_generation, event.object.track_id),
            &event,
            false,
        );
    }

    fn publish_generation_event(&self, event: AiGenerationEventDataV1) {
        let subject = event
            .ai_generation
            .as_deref()
            .or(event.previous_ai_generation.as_deref())
            .map(|generation| format!("generation/{generation}"))
            .unwrap_or_else(|| "generation/none".into());
        self.inner
            .result_bus
            .publish(GENERATION_TYPE, subject, &event, false);
    }

    fn finish_result_generation(&self, reason: &str) {
        let finish = self
            .inner
            .lifecycle
            .lock()
            .unwrap()
            .finish(reason, now_ms());
        for event in finish.exited {
            self.publish_track_event(TRACK_EXITED_TYPE, event);
        }
        if let Some(event) = finish.generation {
            self.publish_generation_event(event);
        }
    }

    pub async fn list_models(&self) -> anyhow::Result<Vec<AiModelInfo>> {
        let state = self.inner.state.lock().await.clone();
        let mut referenced = HashSet::new();
        for project in [state.active_project, state.last_good_project]
            .into_iter()
            .flatten()
        {
            if let Ok(document) = self.get_project(&project).await {
                for file in document.manifest.referenced_files() {
                    referenced.insert(file.to_owned());
                }
            }
        }
        let mut reader = tokio::fs::read_dir(&self.inner.models).await?;
        let mut models = Vec::new();
        while let Some(entry) = reader.next_entry().await? {
            if !entry.file_type().await?.is_file() {
                continue;
            }
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.ends_with(".part") {
                continue;
            }
            let data = tokio::fs::read(entry.path()).await?;
            models.push(AiModelInfo {
                name: name.clone(),
                bytes: data.len() as u64,
                sha256: hex(&Sha256::digest(&data)),
                active: referenced.contains(&name),
            });
        }
        models.sort_by(|left, right| left.name.cmp(&right.name));
        Ok(models)
    }

    pub async fn put_model(&self, name: &str, data: &[u8]) -> anyhow::Result<AiModelInfo> {
        validate_name(name)?;
        anyhow::ensure!(!data.is_empty(), "model upload is empty");
        anyhow::ensure!(
            data.len() as u64 <= self.inner.config.max_model_bytes,
            "model exceeds configured size limit"
        );
        let target = self.inner.models.join(name);
        let part = self
            .inner
            .models
            .join(format!(".{name}.{}.part", Uuid::new_v4()));
        write_synced(&part, data).await?;
        tokio::fs::rename(&part, &target).await?;
        Ok(AiModelInfo {
            name: name.into(),
            bytes: data.len() as u64,
            sha256: hex(&Sha256::digest(data)),
            active: false,
        })
    }

    pub async fn delete_model(&self, name: &str) -> anyhow::Result<()> {
        validate_name(name)?;
        let state = self.inner.state.lock().await.clone();
        for project in [state.active_project, state.last_good_project]
            .into_iter()
            .flatten()
        {
            let document = self.get_project(&project).await?;
            if document.manifest.referenced_files().contains(&name) {
                anyhow::bail!("model is referenced by active or last-good project");
            }
        }
        tokio::fs::remove_file(self.inner.models.join(name)).await?;
        Ok(())
    }

    pub async fn set_osd_mode(&self, mode: OsdMode) -> anyhow::Result<OsdMode> {
        self.apply_osd_mode(mode, true).await
    }

    async fn apply_osd_mode(&self, mode: OsdMode, persist: bool) -> anyhow::Result<OsdMode> {
        if mode == OsdMode::EmbeddedRgn {
            let capability = self
                .inner
                .hub
                .media_request("probe_region_capability", json!({}))
                .await?;
            anyhow::ensure!(
                capability
                    .get("implemented")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                "board does not expose a usable VENC RGN backend"
            );
            self.inner.status.write().unwrap().rgn_capability = Some(capability);
        }
        self.inner
            .hub
            .media_request("set_osd_mode", json!({"mode": mode}))
            .await?;
        if persist {
            let mut state = self.inner.state.lock().await;
            state.osd_mode = mode;
            self.persist_state(&state).await?;
        }
        {
            let mut status = self.inner.status.write().unwrap();
            status.osd_mode = mode;
            if mode != OsdMode::EmbeddedRgn {
                status.rgn_capability = None;
            }
        }
        if mode != OsdMode::EmbeddedRgn {
            self.inner.metadata_history.write().unwrap().clear();
        }
        Ok(mode)
    }

    async fn persist_state(&self, state: &PersistedAiState) -> anyhow::Result<()> {
        let part = self.inner.root.join("state.json.part");
        tokio::fs::write(&part, serde_json::to_vec_pretty(state)?).await?;
        tokio::fs::rename(part, self.inner.root.join("state.json")).await?;
        Ok(())
    }

    fn set_error(&self, error: String) {
        warn!(%error, "AI manager error");
        self.inner.status.write().unwrap().last_error = Some(error.clone());
        let _ = self
            .inner
            .events
            .send(ServerEvent::new("ai_error", json!({"error": error})));
    }

    fn record_log(&self, line: String) {
        info!(message = %line, "ai_worker");
        let _ = self
            .inner
            .events
            .send(ServerEvent::new("ai_log", json!({"line": line})));
    }
}

enum ProcessMessage {
    Ready,
    Result(Value, Arc<AiFrame>),
    Error(String),
    Log(String),
    Exited(Option<i32>, Option<i32>),
}

fn spawn_frame_forwarder(
    mut stream: tokio::net::UnixStream,
    mut input: tokio::sync::watch::Receiver<Option<Arc<AiFrame>>>,
    frames: Arc<Mutex<VecDeque<Arc<AiFrame>>>>,
    messages: mpsc::Sender<ProcessMessage>,
) {
    tokio::spawn(async move {
        let mut sequence = None;
        loop {
            if input.changed().await.is_err() {
                break;
            }
            let Some(frame) = input.borrow_and_update().clone() else {
                continue;
            };
            if sequence == Some(frame.sequence) {
                continue;
            }
            sequence = Some(frame.sequence);
            {
                let mut history = frames.lock().await;
                if history.len() == 8 {
                    history.pop_front();
                }
                history.push_back(frame.clone());
            }
            if let Err(error) = stream.write_all(&encode_ai_frame(&frame)).await {
                let _ = messages
                    .send(ProcessMessage::Error(format!(
                        "write AIPF to AI worker: {error}"
                    )))
                    .await;
                break;
            }
        }
    });
}

fn spawn_result_reader(
    mut stream: tokio::net::UnixStream,
    frames: Arc<Mutex<VecDeque<Arc<AiFrame>>>>,
    messages: mpsc::Sender<ProcessMessage>,
) {
    tokio::spawn(async move {
        loop {
            let length = match stream.read_u32().await {
                Ok(value) => value as usize,
                Err(error) if error.kind() == std::io::ErrorKind::UnexpectedEof => break,
                Err(error) => {
                    let _ = messages
                        .send(ProcessMessage::Error(format!("read AIPR header: {error}")))
                        .await;
                    break;
                }
            };
            if length == 0 || length > MAX_RESULT_BYTES {
                let _ = messages
                    .send(ProcessMessage::Error(format!(
                        "invalid AIPR payload length {length}"
                    )))
                    .await;
                break;
            }
            let mut payload = vec![0_u8; length];
            if let Err(error) = stream.read_exact(&mut payload).await {
                let _ = messages
                    .send(ProcessMessage::Error(format!("read AIPR payload: {error}")))
                    .await;
                break;
            }
            let value: Value = match serde_json::from_slice(&payload) {
                Ok(value) => value,
                Err(error) => {
                    let _ = messages
                        .send(ProcessMessage::Error(format!("parse AIPR: {error}")))
                        .await;
                    continue;
                }
            };
            match value.get("type").and_then(Value::as_str) {
                Some("worker_ready") => {
                    let _ = messages.send(ProcessMessage::Ready).await;
                }
                Some("inference_result") => {
                    let sequence = value.get("sequence").and_then(Value::as_u64);
                    let frame = {
                        let history = frames.lock().await;
                        history
                            .iter()
                            .find(|frame| Some(frame.sequence) == sequence)
                            .cloned()
                    };
                    if let Some(frame) = frame {
                        let _ = messages.send(ProcessMessage::Result(value, frame)).await;
                    }
                }
                Some("worker_error") => {
                    let error = value
                        .get("error")
                        .and_then(Value::as_str)
                        .unwrap_or("AI worker error")
                        .to_owned();
                    let _ = messages.send(ProcessMessage::Error(error)).await;
                }
                _ => {
                    let _ = messages
                        .send(ProcessMessage::Error("unknown AIPR message".into()))
                        .await;
                }
            }
        }
    });
}

fn spawn_stderr_reader(
    stderr: tokio::process::ChildStderr,
    messages: mpsc::Sender<ProcessMessage>,
) {
    tokio::spawn(async move {
        use tokio::io::{AsyncBufReadExt, BufReader};
        let mut lines = BufReader::new(stderr).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            let _ = messages.send(ProcessMessage::Log(line)).await;
        }
    });
}

fn validate_name(value: &str) -> anyhow::Result<()> {
    anyhow::ensure!(
        !value.is_empty()
            && value.len() <= 128
            && value != "."
            && value != ".."
            && value
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-')),
        "invalid file or project name"
    );
    Ok(())
}

fn hex(value: &[u8]) -> String {
    const CHARS: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(value.len() * 2);
    for byte in value {
        output.push(CHARS[(byte >> 4) as usize] as char);
        output.push(CHARS[(byte & 0x0f) as usize] as char);
    }
    output
}

async fn write_synced(path: &Path, data: &[u8]) -> anyhow::Result<()> {
    let mut file = tokio::fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(path)
        .await?;
    file.write_all(data).await?;
    file.sync_all().await?;
    Ok(())
}

fn default_entry() -> String {
    "main.lua".into()
}
fn default_manifest_schema_version() -> u32 {
    1
}
fn default_algorithm() -> String {
    "yolov5".into()
}
fn default_algorithm_options() -> Value {
    json!({})
}
fn default_threshold() -> f32 {
    0.25
}
fn default_nms_threshold() -> f32 {
    0.45
}
fn default_max_detections() -> usize {
    32
}

#[cfg(test)]
mod tests {
    use super::*;

    fn metadata(x: f64) -> Arc<AiMetadata> {
        Arc::new(AiMetadata {
            version: 1,
            generation: "ai-generation".into(),
            sequence: 1,
            pts: 10,
            main_width: 1920,
            main_height: 1080,
            inference_us: 20_000,
            detections: vec![AiDetection {
                track_id: 7,
                class_id: 0,
                label: "person".into(),
                confidence: 0.9,
                x,
                y: 0.2,
                width: 0.2,
                height: 0.3,
            }],
            annotations: vec![],
        })
    }

    #[test]
    fn embedded_regions_interpolate_and_extrapolate_by_track() {
        let start = Instant::now();
        let latest_at = start + Duration::from_millis(200);
        let history = VecDeque::from([
            TimedMetadata {
                metadata: metadata(0.1),
                received_at: start,
            },
            TimedMetadata {
                metadata: metadata(0.3),
                received_at: latest_at,
            },
        ]);

        let interpolated = render_regions(&history, latest_at, Duration::from_millis(500)).unwrap();
        assert!((interpolated.regions[0].x - 0.2).abs() < 0.0001);

        let extrapolated = render_regions(
            &history,
            latest_at + Duration::from_millis(200),
            Duration::from_millis(500),
        )
        .unwrap();
        assert!((extrapolated.regions[0].x - 0.4).abs() < 0.0001);
    }

    #[test]
    fn embedded_regions_expire_after_configured_ttl() {
        let now = Instant::now();
        let history = VecDeque::from([TimedMetadata {
            metadata: metadata(0.1),
            received_at: now,
        }]);
        assert!(
            render_regions(
                &history,
                now + Duration::from_millis(501),
                Duration::from_millis(500),
            )
            .is_none()
        );
    }

    #[test]
    fn runtime_guard_requires_explicit_acknowledgement() {
        let mut manifest = AiProjectManifest {
            schema_version: 2,
            id: "guarded-model".into(),
            name: "Guarded model".into(),
            entry: "main.lua".into(),
            algorithm: "yolov5".into(),
            model: "guarded.rknn".into(),
            labels: String::new(),
            files: BTreeMap::new(),
            options: json!({
                "runtime_guard": "model/runtime compatibility has not been verified",
                "runtime_guard_ack": false
            }),
            input: AiInputConfig::default(),
            threshold: 0.25,
            nms_threshold: 0.45,
            max_detections: 32,
            class_filter: Vec::new(),
        };

        manifest.validate().unwrap();
        let error = manifest.validate_runtime_guard().unwrap_err().to_string();
        assert!(error.contains("runtime guard"));

        manifest.options["runtime_guard_ack"] = Value::Bool(true);
        manifest.validate_runtime_guard().unwrap();
    }
}
