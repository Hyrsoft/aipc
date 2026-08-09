use serde::{Deserialize, Serialize};
use std::net::{IpAddr, SocketAddr};
use std::path::{Component, Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct RuntimeConfig {
    pub generation: String,
    pub duration_sec: i32,
    pub metrics_interval_ms: i32,
    pub warning_timeout_count: i32,
    pub stalled_timeout_count: i32,
    pub fatal_timeout_count: i32,
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self {
            generation: "daemon-managed".into(),
            duration_sec: 0,
            metrics_interval_ms: 5_000,
            warning_timeout_count: 3,
            stalled_timeout_count: 10,
            fatal_timeout_count: 30,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct IspConfig {
    pub iq_dir: String,
    pub camera_id: i32,
}

impl Default for IspConfig {
    fn default() -> Self {
        Self {
            iq_dir: "/etc/iqfiles".into(),
            camera_id: 0,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct ViConfig {
    pub device_id: i32,
    pub pipe_id: i32,
    pub channel_id: i32,
    #[serde(default = "default_two")]
    pub buffer_count: i32,
}

impl Default for ViConfig {
    fn default() -> Self {
        Self {
            device_id: 0,
            pipe_id: 0,
            channel_id: 0,
            buffer_count: 2,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
#[serde(default, deny_unknown_fields)]
pub struct VpssConfig {
    pub group_id: i32,
    pub channel_id: i32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct AiInputConfig {
    pub enabled: bool,
    pub channel_id: i32,
    pub width: i32,
    pub height: i32,
    pub fps: i32,
    pub pixel_format: String,
    pub fit_mode: String,
    pub buffer_count: i32,
    pub depth: i32,
}

impl Default for AiInputConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            channel_id: 1,
            width: 640,
            height: 360,
            fps: 10,
            pixel_format: "nv12".into(),
            fit_mode: "contain".into(),
            buffer_count: 2,
            depth: 1,
        }
    }
}

impl AiInputConfig {
    pub fn validate(&self) -> Vec<String> {
        let mut errors = Vec::new();
        range(&mut errors, "ai_input.channel_id", self.channel_id, 0, 3);
        range(&mut errors, "ai_input.width", self.width, 384, 4096);
        range(&mut errors, "ai_input.height", self.height, 256, 4096);
        range(&mut errors, "ai_input.fps", self.fps, 1, 120);
        range(
            &mut errors,
            "ai_input.buffer_count",
            self.buffer_count,
            1,
            8,
        );
        range(&mut errors, "ai_input.depth", self.depth, 0, 8);
        if self.width % 2 != 0 || self.height % 2 != 0 {
            errors.push("ai_input width and height must be even for NV12".into());
        }
        if self.pixel_format != "nv12" {
            errors.push("ai_input.pixel_format must be nv12".into());
        }
        if !matches!(self.fit_mode.as_str(), "stretch" | "contain" | "cover") {
            errors.push("ai_input.fit_mode must be stretch, contain, or cover".into());
        }
        errors
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct VideoConfig {
    pub enabled: bool,
    pub width: i32,
    pub height: i32,
    pub fps: i32,
    pub bitrate_kbps: i32,
    pub gop: i32,
    pub venc_channel_id: i32,
    pub stream_buffer_count: i32,
    pub output_path: String,
}

impl Default for VideoConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            width: 1920,
            height: 1080,
            fps: 30,
            bitrate_kbps: 4096,
            gop: 30,
            venc_channel_id: 0,
            stream_buffer_count: 3,
            output_path: String::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct AudioConfig {
    pub enabled: bool,
    pub card_name: String,
    pub device_id: i32,
    pub channel_id: i32,
    pub aenc_channel_id: i32,
    pub device_sample_rate: i32,
    pub sample_rate: i32,
    pub device_channels: i32,
    pub channels: i32,
    pub bit_width: i32,
    pub frame_samples: i32,
    pub bitrate: i32,
    pub buffer_count: i32,
    pub output_path: String,
}

impl Default for AudioConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            card_name: "hw:0,0".into(),
            device_id: 0,
            channel_id: 0,
            aenc_channel_id: 0,
            device_sample_rate: 8000,
            sample_rate: 8000,
            device_channels: 2,
            channels: 1,
            bit_width: 16,
            frame_samples: 1024,
            bitrate: 64_000,
            buffer_count: 4,
            output_path: String::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
#[serde(default, deny_unknown_fields)]
pub struct WorkerConfig {
    pub runtime: RuntimeConfig,
    pub isp: IspConfig,
    pub vi: ViConfig,
    pub vpss: VpssConfig,
    pub ai_input: AiInputConfig,
    pub video: VideoConfig,
    pub audio: AudioConfig,
}

impl WorkerConfig {
    pub fn validate(&self) -> Vec<String> {
        let mut errors = Vec::new();
        if !self.video.enabled {
            errors.push("video.enabled must be true in media worker v1".into());
        }
        range(&mut errors, "video.width", self.video.width, 160, 4096);
        range(&mut errors, "video.height", self.video.height, 120, 4096);
        if self.video.width % 2 != 0 || self.video.height % 2 != 0 {
            errors.push("video width and height must be even for NV12".into());
        }
        range(&mut errors, "video.fps", self.video.fps, 1, 60);
        range(
            &mut errors,
            "video.bitrate_kbps",
            self.video.bitrate_kbps,
            64,
            50_000,
        );
        range(&mut errors, "video.gop", self.video.gop, 1, 300);
        range(&mut errors, "vi.buffer_count", self.vi.buffer_count, 1, 16);
        range(
            &mut errors,
            "video.stream_buffer_count",
            self.video.stream_buffer_count,
            1,
            16,
        );
        if self.ai_input.enabled {
            range(
                &mut errors,
                "ai_input.channel_id",
                self.ai_input.channel_id,
                0,
                3,
            );
            range(
                &mut errors,
                "ai_input.width",
                self.ai_input.width,
                384,
                4096,
            );
            range(
                &mut errors,
                "ai_input.height",
                self.ai_input.height,
                256,
                4096,
            );
            range(
                &mut errors,
                "ai_input.fps",
                self.ai_input.fps,
                1,
                self.video.fps,
            );
            range(
                &mut errors,
                "ai_input.buffer_count",
                self.ai_input.buffer_count,
                1,
                8,
            );
            range(&mut errors, "ai_input.depth", self.ai_input.depth, 0, 8);
            if self.ai_input.width % 2 != 0 || self.ai_input.height % 2 != 0 {
                errors.push("ai_input width and height must be even for NV12".into());
            }
            if self.ai_input.channel_id == self.vpss.channel_id {
                errors.push("ai_input.channel_id must differ from vpss.channel_id".into());
            }
            if self.ai_input.pixel_format != "nv12" {
                errors.push("ai_input.pixel_format must be nv12".into());
            }
            if !matches!(
                self.ai_input.fit_mode.as_str(),
                "stretch" | "contain" | "cover"
            ) {
                errors.push("ai_input.fit_mode must be stretch, contain, or cover".into());
            }
        }
        if self.runtime.warning_timeout_count >= self.runtime.stalled_timeout_count
            || self.runtime.stalled_timeout_count >= self.runtime.fatal_timeout_count
        {
            errors.push("timeout counts must satisfy warning < stalled < fatal".into());
        }
        if self.audio.enabled {
            if self.audio.card_name.is_empty() {
                errors.push("audio.card_name is required".into());
            }
            if self.audio.sample_rate != 8000
                || self.audio.channels != 1
                || self.audio.bit_width != 16
                || self.audio.bitrate != 64_000
            {
                errors.push("G711A requires 8000Hz, mono, 16-bit input and 64000 bitrate".into());
            }
            range(
                &mut errors,
                "audio.device_sample_rate",
                self.audio.device_sample_rate,
                8000,
                48000,
            );
            range(
                &mut errors,
                "audio.device_channels",
                self.audio.device_channels,
                1,
                2,
            );
            range(
                &mut errors,
                "audio.frame_samples",
                self.audio.frame_samples,
                80,
                2048,
            );
            range(
                &mut errors,
                "audio.buffer_count",
                self.audio.buffer_count,
                2,
                16,
            );
        }
        for value in [
            self.isp.camera_id,
            self.vi.device_id,
            self.vi.pipe_id,
            self.vi.channel_id,
            self.vpss.group_id,
            self.vpss.channel_id,
            self.video.venc_channel_id,
        ] {
            if !(0..=63).contains(&value) {
                errors.push("video hardware channel IDs must be in [0, 63]".into());
                break;
            }
        }
        if self.audio.enabled
            && [
                self.audio.device_id,
                self.audio.channel_id,
                self.audio.aenc_channel_id,
            ]
            .iter()
            .any(|value| *value < 0)
        {
            errors.push("audio hardware channel IDs must be non-negative".into());
        }
        errors
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct UiConfig {
    pub project_name: String,
    pub console_name: String,
    pub platform_name: String,
    pub board_name: String,
    pub project_url: String,
    pub documentation_url: String,
    pub visiong_url: String,
    pub license_name: String,
    pub license_url: String,
}

impl Default for UiConfig {
    fn default() -> Self {
        Self {
            project_name: "AIPC".into(),
            console_name: "Media Console".into(),
            platform_name: "RV1106".into(),
            board_name: "Luckfox Pico Ultra W".into(),
            project_url: "https://github.com/haoyn231/aipc".into(),
            documentation_url: "https://github.com/haoyn231/aipc/tree/main/docs".into(),
            visiong_url: "https://github.com/yiex/visiong".into(),
            license_name: "Apache-2.0".into(),
            license_url: "https://github.com/haoyn231/aipc/blob/main/LICENSE".into(),
        }
    }
}

impl UiConfig {
    fn validate(&self) -> anyhow::Result<()> {
        for (name, value) in [
            ("ui.project_name", &self.project_name),
            ("ui.console_name", &self.console_name),
            ("ui.platform_name", &self.platform_name),
            ("ui.board_name", &self.board_name),
            ("ui.license_name", &self.license_name),
        ] {
            anyhow::ensure!(
                !value.trim().is_empty() && value.len() <= 128,
                "{name} must contain 1-128 characters"
            );
        }
        for (name, value) in [
            ("ui.project_url", &self.project_url),
            ("ui.documentation_url", &self.documentation_url),
            ("ui.visiong_url", &self.visiong_url),
            ("ui.license_url", &self.license_url),
        ] {
            anyhow::ensure!(
                value.starts_with("https://") || value.starts_with("http://"),
                "{name} must be an HTTP(S) URL"
            );
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct DaemonConfig {
    pub bind: String,
    pub worker_path: PathBuf,
    pub web_dir: PathBuf,
    pub data_dir: PathBuf,
    pub runtime_dir: PathBuf,
    pub seed_config: PathBuf,
    pub autostart: bool,
    pub startup_timeout_ms: u64,
    pub stop_timeout_ms: u64,
    pub max_restarts: usize,
    pub restart_window_sec: u64,
    pub watchdog: WatchdogConfig,
    pub dependencies: DependencyConfig,
    pub ui: UiConfig,
    pub preview: PreviewConfig,
    pub recording: RecordingConfig,
    pub rtsp: RtspConfig,
    pub webrtc: WebRtcConfig,
    pub ai: AiDaemonConfig,
    pub input: InputConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct InputConfig {
    pub enabled: bool,
    pub active_source: Option<String>,
    pub file_roots: Vec<PathBuf>,
    pub sources: Vec<InputSourceConfig>,
    pub processor_path: PathBuf,
    pub processor_startup_timeout_ms: u64,
    pub reconnect_initial_ms: u64,
    pub reconnect_max_ms: u64,
    pub processor: AiInputConfig,
    pub processor_vdec_channel: i32,
    pub processor_vpss_group: i32,
}

impl Default for InputConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            active_source: None,
            file_roots: vec!["../media".into()],
            sources: Vec::new(),
            processor_path: "video_decode_worker".into(),
            processor_startup_timeout_ms: 10_000,
            reconnect_initial_ms: 500,
            reconnect_max_ms: 10_000,
            processor: AiInputConfig::default(),
            processor_vdec_channel: 0,
            processor_vpss_group: 1,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct InputSourceConfig {
    pub id: String,
    #[serde(default)]
    pub ai_sidecar: bool,
    #[serde(flatten)]
    pub source: InputSourceKind,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum InputSourceKind {
    File {
        path: PathBuf,
        #[serde(default)]
        loop_playback: bool,
        #[serde(default = "default_h264_fps")]
        fps: u32,
        #[serde(default = "default_true")]
        realtime: bool,
    },
    Rtsp {
        url: String,
        #[serde(default)]
        username: Option<String>,
        #[serde(default)]
        password: Option<String>,
        #[serde(default = "default_max_video_width")]
        max_width: u32,
        #[serde(default = "default_max_video_height")]
        max_height: u32,
        #[serde(default = "default_max_video_fps")]
        max_fps: u32,
    },
}

impl InputConfig {
    pub fn validate(&self) -> anyhow::Result<()> {
        anyhow::ensure!(
            self.reconnect_initial_ms > 0
                && self.reconnect_initial_ms <= self.reconnect_max_ms
                && self.reconnect_max_ms <= 300_000,
            "input reconnect delays are invalid"
        );
        anyhow::ensure!(
            self.processor_startup_timeout_ms > 0,
            "input.processor_startup_timeout_ms must be greater than zero"
        );
        anyhow::ensure!(
            (0..=3).contains(&self.processor.channel_id),
            "input.processor.channel_id must be in [0, 3]"
        );
        if self.processor.enabled {
            anyhow::ensure!(
                self.processor.validate().is_empty(),
                "input.processor configuration is invalid"
            );
        }
        anyhow::ensure!(
            (0..=7).contains(&self.processor_vdec_channel),
            "input.processor_vdec_channel must be in [0, 7]"
        );
        anyhow::ensure!(
            (0..=7).contains(&self.processor_vpss_group),
            "input.processor_vpss_group must be in [0, 7]"
        );
        for root in &self.file_roots {
            anyhow::ensure!(root.is_absolute(), "input.file_roots must be absolute");
        }
        let mut ids = std::collections::HashSet::new();
        for source in &self.sources {
            anyhow::ensure!(
                !source.id.is_empty()
                    && source.id.len() <= 64
                    && source.id.bytes().all(|byte| {
                        byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-')
                    }),
                "input source id must contain 1-64 safe identifier characters"
            );
            anyhow::ensure!(ids.insert(source.id.clone()), "duplicate input source id");
            match &source.source {
                InputSourceKind::File { path, fps, .. } => {
                    anyhow::ensure!(path.is_absolute(), "input file path must be absolute");
                    anyhow::ensure!(
                        (1..=120).contains(fps),
                        "input file fps must be in [1, 120]"
                    );
                    let normalized_path = normalize_path(path);
                    anyhow::ensure!(
                        self.file_roots
                            .iter()
                            .map(|root| normalize_path(root))
                            .any(|root| normalized_path.strip_prefix(root).is_ok()),
                        "input file path is outside input.file_roots"
                    );
                    let extension = path
                        .extension()
                        .and_then(|value| value.to_str())
                        .unwrap_or_default()
                        .to_ascii_lowercase();
                    anyhow::ensure!(
                        matches!(extension.as_str(), "mp4" | "h264" | "264"),
                        "input file must be MP4 or Annex-B H264"
                    );
                }
                InputSourceKind::Rtsp {
                    url,
                    max_width,
                    max_height,
                    max_fps,
                    ..
                } => {
                    anyhow::ensure!(
                        url.starts_with("rtsp://"),
                        "input RTSP URL must start with rtsp://"
                    );
                    anyhow::ensure!(
                        (160..=8192).contains(max_width)
                            && (120..=8192).contains(max_height)
                            && (1..=120).contains(max_fps),
                        "input RTSP resolution/FPS limits are invalid"
                    );
                }
            }
        }
        if self.enabled {
            let active = self
                .active_source
                .as_deref()
                .ok_or_else(|| anyhow::anyhow!("input.active_source is required"))?;
            anyhow::ensure!(
                self.sources.iter().any(|source| source.id == active),
                "input.active_source does not exist"
            );
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct DependencyConfig {
    pub enabled: bool,
    pub root: PathBuf,
    pub max_upload_bytes: u64,
}

impl Default for DependencyConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            root: "../data/dependencies".into(),
            max_upload_bytes: 32 * 1024 * 1024,
        }
    }
}

impl DependencyConfig {
    pub fn validate(&self) -> anyhow::Result<()> {
        anyhow::ensure!(
            self.root.is_absolute(),
            "dependencies.root must be absolute"
        );
        anyhow::ensure!(
            (1024..=256 * 1024 * 1024).contains(&self.max_upload_bytes),
            "dependencies.max_upload_bytes must be in [1024, 268435456]"
        );
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct WatchdogConfig {
    pub enabled: bool,
    pub required: bool,
    pub device: PathBuf,
    pub timeout_sec: u32,
    pub feed_interval_ms: u64,
}

impl Default for WatchdogConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            required: false,
            device: "/dev/watchdog".into(),
            timeout_sec: 30,
            feed_interval_ms: 5_000,
        }
    }
}

impl WatchdogConfig {
    pub fn validate(&self) -> anyhow::Result<()> {
        anyhow::ensure!(
            self.device.is_absolute(),
            "watchdog.device must be absolute"
        );
        anyhow::ensure!(
            (2..=300).contains(&self.timeout_sec),
            "watchdog.timeout_sec must be in [2, 300]"
        );
        anyhow::ensure!(
            (250..self.timeout_sec as u64 * 500).contains(&self.feed_interval_ms),
            "watchdog.feed_interval_ms must be at least 250 ms and less than half the timeout"
        );
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct AiDaemonConfig {
    pub enabled: bool,
    pub worker_path: PathBuf,
    pub startup_timeout_ms: u64,
    pub max_model_bytes: u64,
    pub result_ttl_ms: u64,
    pub source_id: String,
    pub result_replay_capacity: usize,
    pub track_confirmations: usize,
    pub track_lost_timeout_ms: u64,
    pub track_update_interval_ms: u64,
}

impl Default for AiDaemonConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            worker_path: "ai_worker".into(),
            startup_timeout_ms: 30_000,
            max_model_bytes: 128 * 1024 * 1024,
            result_ttl_ms: 500,
            source_id: "camera0".into(),
            result_replay_capacity: 256,
            track_confirmations: 2,
            track_lost_timeout_ms: 500,
            track_update_interval_ms: 500,
        }
    }
}

impl AiDaemonConfig {
    pub fn validate(&self) -> anyhow::Result<()> {
        anyhow::ensure!(
            !self.source_id.is_empty()
                && self.source_id.len() <= 64
                && self.source_id.bytes().all(|byte| {
                    byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-')
                }),
            "ai.source_id must contain 1-64 safe identifier characters"
        );
        anyhow::ensure!(
            (1..=4096).contains(&self.result_replay_capacity),
            "ai.result_replay_capacity must be in [1, 4096]"
        );
        anyhow::ensure!(
            (1..=16).contains(&self.track_confirmations),
            "ai.track_confirmations must be in [1, 16]"
        );
        anyhow::ensure!(
            (50..=60_000).contains(&self.track_lost_timeout_ms),
            "ai.track_lost_timeout_ms must be in [50, 60000]"
        );
        anyhow::ensure!(
            (50..=60_000).contains(&self.track_update_interval_ms),
            "ai.track_update_interval_ms must be in [50, 60000]"
        );
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct PreviewConfig {
    pub enabled: bool,
    pub max_clients: usize,
    pub max_frame_bytes: usize,
    pub max_audio_frame_bytes: usize,
    pub broadcast_capacity: usize,
}

impl Default for PreviewConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            max_clients: 5,
            max_frame_bytes: 4 * 1024 * 1024,
            max_audio_frame_bytes: 64 * 1024,
            broadcast_capacity: 32,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct RecordingConfig {
    pub enabled: bool,
    pub directory: PathBuf,
    pub allowed_roots: Vec<PathBuf>,
    pub queue_capacity: usize,
    pub max_duration_sec: u64,
    pub max_file_bytes: u64,
    pub min_free_bytes: u64,
    pub max_export_files: usize,
}

impl Default for RecordingConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            directory: "../recordings".into(),
            allowed_roots: vec!["../recordings".into()],
            queue_capacity: 256,
            max_duration_sec: 3600,
            max_file_bytes: 2 * 1024 * 1024 * 1024,
            min_free_bytes: 64 * 1024 * 1024,
            max_export_files: 100,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct RtspConfig {
    pub enabled: bool,
    pub bind: String,
    pub path: String,
    pub max_clients: usize,
    pub mtu: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct WebRtcConfig {
    pub enabled: bool,
    pub bind: String,
    pub advertised_ip: Option<IpAddr>,
    pub max_clients: usize,
    pub mtu: usize,
    pub connect_timeout_ms: u64,
    pub idle_timeout_ms: u64,
}

impl Default for WebRtcConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            bind: "0.0.0.0:10000".into(),
            advertised_ip: None,
            max_clients: 4,
            mtu: 1200,
            connect_timeout_ms: 10_000,
            idle_timeout_ms: 30_000,
        }
    }
}

impl WebRtcConfig {
    pub fn validate(&self) -> anyhow::Result<()> {
        let bind: SocketAddr = self
            .bind
            .parse()
            .map_err(|_| anyhow::anyhow!("webrtc.bind must be a socket address"))?;
        if bind.port() == 0 {
            anyhow::bail!("webrtc.bind port must be non-zero");
        }
        if !bind.is_ipv4() {
            anyhow::bail!("webrtc.bind must use IPv4 in the LAN-only release");
        }
        if self
            .advertised_ip
            .is_some_and(|value| value.is_unspecified())
        {
            anyhow::bail!("webrtc.advertised_ip must not be unspecified");
        }
        if self.advertised_ip.is_some_and(|value| !value.is_ipv4()) {
            anyhow::bail!("webrtc.advertised_ip must use IPv4 in the LAN-only release");
        }
        if self.max_clients == 0 {
            anyhow::bail!("webrtc.max_clients must be greater than zero");
        }
        if !(656..=1500).contains(&self.mtu) {
            anyhow::bail!("webrtc.mtu must be in [656, 1500]");
        }
        if self.connect_timeout_ms == 0 || self.idle_timeout_ms == 0 {
            anyhow::bail!("webrtc timeouts must be greater than zero");
        }
        Ok(())
    }
}

impl Default for RtspConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            bind: "0.0.0.0:8554".into(),
            path: "/live".into(),
            max_clients: 4,
            mtu: 1200,
        }
    }
}

impl Default for DaemonConfig {
    fn default() -> Self {
        Self {
            bind: "0.0.0.0:8080".into(),
            worker_path: "media_worker".into(),
            web_dir: "../www".into(),
            data_dir: "../data".into(),
            runtime_dir: "/tmp/aipc".into(),
            seed_config: "../config/media_worker.example.json".into(),
            autostart: true,
            startup_timeout_ms: 15_000,
            stop_timeout_ms: 5_000,
            max_restarts: 5,
            restart_window_sec: 300,
            watchdog: WatchdogConfig::default(),
            dependencies: DependencyConfig::default(),
            ui: UiConfig::default(),
            preview: PreviewConfig::default(),
            recording: RecordingConfig::default(),
            rtsp: RtspConfig::default(),
            webrtc: WebRtcConfig::default(),
            ai: AiDaemonConfig::default(),
            input: InputConfig::default(),
        }
    }
}

impl DaemonConfig {
    pub async fn load(path: Option<&Path>, executable_dir: &Path) -> anyhow::Result<Self> {
        let mut config = if let Some(path) = path {
            let data = tokio::fs::read(path).await?;
            serde_json::from_slice(&data)?
        } else {
            Self::default()
        };
        config.worker_path = resolve(executable_dir, &config.worker_path);
        config.web_dir = resolve(executable_dir, &config.web_dir);
        config.data_dir = resolve(executable_dir, &config.data_dir);
        config.runtime_dir = resolve(executable_dir, &config.runtime_dir);
        config.seed_config = resolve(executable_dir, &config.seed_config);
        config.ai.worker_path = resolve(executable_dir, &config.ai.worker_path);
        config.input.processor_path = resolve(executable_dir, &config.input.processor_path);
        config.input.file_roots = config
            .input
            .file_roots
            .iter()
            .map(|path| normalize_path(&resolve(executable_dir, path)))
            .collect();
        for source in &mut config.input.sources {
            if let InputSourceKind::File { path, .. } = &mut source.source {
                *path = normalize_path(&resolve(executable_dir, path));
            }
        }
        config.dependencies.root = resolve(executable_dir, &config.dependencies.root);
        config.recording.directory = resolve(executable_dir, &config.recording.directory);
        config.recording.allowed_roots = config
            .recording
            .allowed_roots
            .iter()
            .map(|path| resolve(executable_dir, path))
            .collect();
        config.watchdog.validate()?;
        config.dependencies.validate()?;
        config.ai.validate()?;
        config.ui.validate()?;
        config.webrtc.validate()?;
        config.input.validate()?;
        Ok(config)
    }
}

fn resolve(base: &Path, value: &Path) -> PathBuf {
    if value.is_absolute() {
        value.to_path_buf()
    } else {
        base.join(value)
    }
}

/// Normalize `.` and `..` lexically without touching the filesystem. This is
/// used for config validation so a relative default such as `../media` has the
/// same whitelist semantics as its runtime canonical path.
fn normalize_path(path: &Path) -> PathBuf {
    let mut normalized = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                if !normalized.pop() && !path.is_absolute() {
                    normalized.push(component.as_os_str());
                }
            }
            Component::RootDir | Component::Prefix(_) | Component::Normal(_) => {
                normalized.push(component.as_os_str());
            }
        }
    }
    normalized
}

fn range(errors: &mut Vec<String>, name: &str, value: i32, min: i32, max: i32) {
    if value < min || value > max {
        errors.push(format!("{name} must be in [{min}, {max}]"));
    }
}

const fn default_two() -> i32 {
    2
}

const fn default_h264_fps() -> u32 {
    25
}

const fn default_true() -> bool {
    true
}

const fn default_max_video_width() -> u32 {
    4096
}

const fn default_max_video_height() -> u32 {
    4096
}

const fn default_max_video_fps() -> u32 {
    120
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_valid() {
        assert!(WorkerConfig::default().validate().is_empty());
    }

    #[test]
    fn rejects_invalid_video_and_audio() {
        let mut config = WorkerConfig::default();
        config.video.width = 1279;
        config.audio.sample_rate = 16_000;
        let errors = config.validate();
        assert!(errors.iter().any(|item| item.contains("even")));
        assert!(errors.iter().any(|item| item.contains("G711A")));
    }

    #[test]
    fn rejects_unsafe_small_ai_vpss_channels() {
        let mut config = WorkerConfig::default();
        config.ai_input.enabled = true;
        config.ai_input.width = 320;
        assert!(
            config
                .validate()
                .iter()
                .any(|item| item.contains("ai_input.width"))
        );
        config.ai_input.width = 640;
        config.ai_input.height = 240;
        assert!(
            config
                .validate()
                .iter()
                .any(|item| item.contains("ai_input.height"))
        );
    }

    #[test]
    fn validates_webrtc_settings() {
        assert!(WebRtcConfig::default().validate().is_ok());
        let mut config = WebRtcConfig {
            bind: "0.0.0.0:0".into(),
            ..WebRtcConfig::default()
        };
        assert!(config.validate().is_err());
        config.bind = "0.0.0.0:10000".into();
        config.max_clients = 0;
        assert!(config.validate().is_err());
        config.max_clients = 1;
        config.advertised_ip = Some("0.0.0.0".parse().unwrap());
        assert!(config.validate().is_err());
    }

    #[test]
    fn validates_ai_result_settings() {
        assert!(AiDaemonConfig::default().validate().is_ok());
        let config = AiDaemonConfig {
            source_id: "../camera".into(),
            ..AiDaemonConfig::default()
        };
        assert!(config.validate().is_err());
        let config = AiDaemonConfig {
            result_replay_capacity: 0,
            ..AiDaemonConfig::default()
        };
        assert!(config.validate().is_err());
    }

    #[test]
    fn validates_watchdog_settings() {
        assert!(WatchdogConfig::default().validate().is_ok());
        let too_slow = WatchdogConfig {
            timeout_sec: 10,
            feed_interval_ms: 5_000,
            ..WatchdogConfig::default()
        };
        assert!(too_slow.validate().is_err());
        let relative = WatchdogConfig {
            device: "watchdog0".into(),
            ..WatchdogConfig::default()
        };
        assert!(relative.validate().is_err());
    }

    #[test]
    fn validates_ui_platform_and_links() {
        assert!(UiConfig::default().validate().is_ok());
        let config = UiConfig {
            platform_name: "".into(),
            ..UiConfig::default()
        };
        assert!(config.validate().is_err());
        let config = UiConfig {
            project_url: "file:///tmp/aipc".into(),
            ..UiConfig::default()
        };
        assert!(config.validate().is_err());
    }
}
