export interface RuntimeConfig {
  generation: string
  duration_sec: number
  metrics_interval_ms: number
  warning_timeout_count: number
  stalled_timeout_count: number
  fatal_timeout_count: number
}

export interface WorkerConfig {
  runtime: RuntimeConfig
  isp: { iq_dir: string; camera_id: number }
  vi: { device_id: number; pipe_id: number; channel_id: number; buffer_count: number }
  vpss: { group_id: number; channel_id: number }
  video: {
    enabled: boolean
    width: number
    height: number
    fps: number
    bitrate_kbps: number
    gop: number
    venc_channel_id: number
    stream_buffer_count: number
    output_path: string
  }
  audio: {
    enabled: boolean
    card_name: string
    device_id: number
    channel_id: number
    aenc_channel_id: number
    device_sample_rate: number
    sample_rate: number
    device_channels: number
    channels: number
    bit_width: number
    frame_samples: number
    bitrate: number
    buffer_count: number
    output_path: string
  }
}

export interface PersistentState {
  desired: WorkerConfig | null
  active: WorkerConfig | null
  pending: WorkerConfig | null
  last_good: WorkerConfig | null
  last_error: string | null
}

export interface DaemonStatus {
  state: string
  pid: number | null
  generation: string | null
  stage: string | null
  started_at_ms: number | null
  updated_at_ms: number
  restart_count: number
  video_ready: boolean
  audio_ready: boolean
  last_error: string | null
  metrics: WorkerMetrics | null
}

export interface MediaMetrics {
  packets: number
  bytes: number
  keyframes: number
  last_pts: number
  timeouts: number
  errors: number
  bitrate_kbps?: number
  average_bitrate_kbps?: number
}

export interface VideoMetrics extends MediaMetrics {
  fps?: number
  average_fps?: number
  ipc_frames?: number
  ipc_bytes?: number
  ipc_drops?: number
  ipc_errors?: number
}

export interface WorkerMetrics {
  event?: string
  generation?: string
  elapsed_seconds?: number
  monotonic_ms?: number
  schema_version?: number
  video?: VideoMetrics
  audio?: MediaMetrics
}

export interface LogEntry { timestamp_ms: number; stream: string; line: string }
export interface ServerEvent { kind: string; timestamp_ms: number; payload: any }

export interface RecordingSettings {
  enabled: boolean
  directory: string
  allowed_roots: string[]
  queue_capacity: number
  max_duration_sec: number
  max_file_bytes: number
  min_free_bytes: number
  max_export_files: number
}

export type RecordingState = 'idle' | 'waiting_keyframe' | 'recording' | 'finalizing' | 'failed'
export interface RecordingStatus {
  state: RecordingState
  id: string | null
  file_name: string | null
  generation: string | null
  started_at_ms: number | null
  duration_ms: number
  bytes: number
  last_error: string | null
}

export interface RecordingEntry {
  id: string
  file_name: string
  created_at_ms: number
  duration_ms: number
  bytes: number
  width: number
  height: number
  fps: number
  generation: string
}

export interface RecordingList { items: RecordingEntry[]; total: number; offset: number; limit: number }
export interface RtspStatus { enabled: boolean; listening: boolean; bind: string; path: string; clients: number; max_clients: number; last_error: string | null }
