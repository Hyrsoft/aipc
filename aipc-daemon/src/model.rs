use crate::config::WorkerConfig;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(rename_all = "snake_case")]
pub enum ProcessState {
    #[default]
    Stopped,
    Starting,
    Running,
    Stopping,
    Backoff,
    RollingBack,
    Failed,
}

impl ProcessState {
    pub fn is_transitioning(&self) -> bool {
        matches!(self, Self::Starting | Self::Stopping | Self::RollingBack)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct DaemonStatus {
    pub state: ProcessState,
    pub pid: Option<u32>,
    pub generation: Option<String>,
    pub stage: Option<String>,
    pub started_at_ms: Option<u64>,
    pub updated_at_ms: u64,
    pub restart_count: usize,
    pub video_ready: bool,
    pub audio_ready: bool,
    pub last_error: Option<String>,
    pub metrics: Option<Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct PersistentState {
    pub schema_version: u32,
    pub desired: Option<WorkerConfig>,
    pub active: Option<WorkerConfig>,
    pub pending: Option<WorkerConfig>,
    pub last_good: Option<WorkerConfig>,
    pub last_error: Option<String>,
}

impl PersistentState {
    pub fn new(seed: WorkerConfig) -> Self {
        Self {
            schema_version: 1,
            desired: Some(seed.clone()),
            active: None,
            pending: None,
            last_good: Some(seed),
            last_error: None,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerEvent {
    pub kind: String,
    pub timestamp_ms: u64,
    pub payload: Value,
}

impl ServerEvent {
    pub fn new(kind: impl Into<String>, payload: Value) -> Self {
        Self {
            kind: kind.into(),
            timestamp_ms: now_ms(),
            payload,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LogEntry {
    pub timestamp_ms: u64,
    pub stream: String,
    pub line: String,
}

pub fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
