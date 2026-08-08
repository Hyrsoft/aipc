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
  ai_input: {
    enabled: boolean
    channel_id: number
    width: number
    height: number
    fps: number
    pixel_format: 'nv12'
    fit_mode: 'stretch' | 'contain' | 'cover'
    buffer_count: number
    depth: number
  }
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
  audio_file_name: string | null
  audio_bytes: number
  audio_available: boolean
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
  audio_file_name: string | null
  audio_bytes: number
  audio_sample_rate: number
  audio_channels: number
  audio_available: boolean
}

export interface RecordingList { items: RecordingEntry[]; total: number; offset: number; limit: number }
export interface RtspStatus { enabled: boolean; listening: boolean; bind: string; path: string; clients: number; max_clients: number; last_error: string | null }

export type AiOsdMode = 'off' | 'metadata' | 'embedded_rgn'
export interface AiProjectManifest {
  id: string
  name: string
  entry: string
  algorithm: 'yolov5'
  model: string
  labels: string
  input: WorkerConfig['ai_input']
  threshold: number
  nms_threshold: number
  max_detections: number
  class_filter: number[]
}
export interface AiProjectDocument { manifest: AiProjectManifest; script: string }
export interface AiProjectSummary {
  id: string
  name: string
  algorithm: string
  model: string
  input: WorkerConfig['ai_input']
  active: boolean
  last_good: boolean
}
export interface AiModelInfo { name: string; bytes: number; sha256: string; active: boolean }
export interface AiInputStatus {
  generation: string | null
  available: boolean
  frames_received: number
  bytes_received: number
  malformed_frames: number
  last_sequence: number | null
  last_pts: number | null
  last_frame_at_ms: number | null
  width: number | null
  height: number | null
  y_stride: number | null
  last_error: string | null
  config: WorkerConfig['ai_input'] | null
  control_available: boolean
}
export interface AiStatus {
  enabled: boolean
  state: string
  pid: number | null
  generation: string | null
  active_project: string | null
  last_good_project: string | null
  worker_ready: boolean
  first_inference: boolean
  input: AiInputStatus
  results: number
  inference_fps: number
  average_inference_ms: number
  last_result_at_ms: number | null
  last_error: string | null
  osd_mode: AiOsdMode
  rgn_capability: { line: boolean; cover: boolean; backend: string; max_boxes: number; implemented: boolean } | null
  result_bus: {
    stream_id: string
    latest_event_id: string | null
    earliest_replay_event_id: string | null
    published: number
    replay_depth: number
    replay_capacity: number
    lagged_events: number
  }
}
export interface AiDetection {
  track_id: number
  class_id: number
  label: string
  confidence: number
  x: number
  y: number
  width: number
  height: number
}
export interface AiMetadata {
  version: number
  generation: string
  sequence: number
  pts: number
  main_width: number
  main_height: number
  inference_us: number
  detections: AiDetection[]
}

export type AiResultEventType =
  | 'io.aipc.ai.frame.v1'
  | 'io.aipc.ai.track.entered.v1'
  | 'io.aipc.ai.track.updated.v1'
  | 'io.aipc.ai.track.exited.v1'
  | 'io.aipc.ai.stream.gap.v1'
  | 'io.aipc.ai.generation.v1'

export interface AiResultBoundingBox {
  x: number
  y: number
  width: number
  height: number
}

export interface AiResultObject {
  track_id: number
  class_id: number
  label: string
  confidence: number
  bbox: AiResultBoundingBox
}

export interface AiResultFrameInfo {
  width: number
  height: number
  coordinate_space: 'main_normalized_top_left'
}

export interface AiResultInferenceInfo {
  project: string
  algorithm: string
  model: string
  duration_us: number
}

export interface AiFrameResultData {
  schema_version: 1
  source_id: string
  media_generation: string
  ai_generation: string
  sequence: number
  pts_us: number
  published_at_ms: number
  frame: AiResultFrameInfo
  inference: AiResultInferenceInfo
  objects: AiResultObject[]
}

export interface AiTrackResultData extends Omit<AiFrameResultData, 'objects'> {
  object: AiResultObject
  reason: string
}

export interface AiGenerationResultData {
  schema_version: 1
  source_id: string
  media_generation: string | null
  ai_generation: string | null
  previous_media_generation: string | null
  previous_ai_generation: string | null
  state: 'started' | 'stopped'
  reason: string
  published_at_ms: number
}

export interface AiStreamGapResultData {
  schema_version: 1
  source_id: string
  requested_event_id: string | null
  earliest_event_id: string | null
  latest_event_id: string | null
  reason: string
}

export type AiCloudEventData = AiFrameResultData | AiTrackResultData | AiGenerationResultData | AiStreamGapResultData

export interface AiCloudEvent {
  specversion: '1.0'
  id: string
  source: string
  type: AiResultEventType
  subject: string
  time: string
  datacontenttype: 'application/json'
  dataschema: string
  data: AiCloudEventData
}
