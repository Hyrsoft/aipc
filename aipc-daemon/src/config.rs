use serde::{Deserialize, Serialize};
use std::net::{IpAddr, SocketAddr};
use std::path::{Path, PathBuf};

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
            range(&mut errors, "ai_input.width", self.ai_input.width, 2, 4096);
            range(
                &mut errors,
                "ai_input.height",
                self.ai_input.height,
                2,
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
    pub preview: PreviewConfig,
    pub recording: RecordingConfig,
    pub rtsp: RtspConfig,
    pub webrtc: WebRtcConfig,
    pub ai: AiDaemonConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct AiDaemonConfig {
    pub enabled: bool,
    pub worker_path: PathBuf,
    pub startup_timeout_ms: u64,
    pub max_model_bytes: u64,
    pub result_ttl_ms: u64,
}

impl Default for AiDaemonConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            worker_path: "ai_worker".into(),
            startup_timeout_ms: 30_000,
            max_model_bytes: 128 * 1024 * 1024,
            result_ttl_ms: 500,
        }
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
            preview: PreviewConfig::default(),
            recording: RecordingConfig::default(),
            rtsp: RtspConfig::default(),
            webrtc: WebRtcConfig::default(),
            ai: AiDaemonConfig::default(),
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
        config.recording.directory = resolve(executable_dir, &config.recording.directory);
        config.recording.allowed_roots = config
            .recording
            .allowed_roots
            .iter()
            .map(|path| resolve(executable_dir, path))
            .collect();
        config.webrtc.validate()?;
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

fn range(errors: &mut Vec<String>, name: &str, value: i32, min: i32, max: i32) {
    if value < min || value > max {
        errors.push(format!("{name} must be in [{min}, {max}]"));
    }
}

const fn default_two() -> i32 {
    2
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
}
