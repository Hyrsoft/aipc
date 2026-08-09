use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::{Arc, Mutex as StdMutex};
use std::time::Duration;
use time::OffsetDateTime;
use time::format_description::well_known::Rfc3339;
use tokio::sync::broadcast;
use uuid::Uuid;

pub const FRAME_RESULT_TYPE: &str = "io.aipc.ai.frame.v1";
pub const TRACK_ENTERED_TYPE: &str = "io.aipc.ai.track.entered.v1";
pub const TRACK_UPDATED_TYPE: &str = "io.aipc.ai.track.updated.v1";
pub const TRACK_EXITED_TYPE: &str = "io.aipc.ai.track.exited.v1";
pub const STREAM_GAP_TYPE: &str = "io.aipc.ai.stream.gap.v1";
pub const GENERATION_TYPE: &str = "io.aipc.ai.generation.v1";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiBoundingBoxV1 {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiObjectV1 {
    pub track_id: u64,
    pub class_id: i64,
    pub label: String,
    pub confidence: f64,
    pub bbox: AiBoundingBoxV1,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiAnnotationV1 {
    pub kind: String,
    pub label: String,
    pub confidence: f64,
    pub bbox: AiBoundingBoxV1,
    pub data: Value,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiFrameInfoV1 {
    pub width: u32,
    pub height: u32,
    pub coordinate_space: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiInferenceInfoV1 {
    pub project: String,
    pub algorithm: String,
    pub model: String,
    pub duration_us: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiResultDataV1 {
    pub schema_version: u32,
    pub source_id: String,
    pub media_generation: String,
    pub ai_generation: String,
    pub sequence: u64,
    pub pts_us: u64,
    pub published_at_ms: u64,
    pub frame: AiFrameInfoV1,
    pub inference: AiInferenceInfoV1,
    pub objects: Vec<AiObjectV1>,
    pub annotations: Vec<AiAnnotationV1>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiTrackEventDataV1 {
    pub schema_version: u32,
    pub source_id: String,
    pub media_generation: String,
    pub ai_generation: String,
    pub sequence: u64,
    pub pts_us: u64,
    pub published_at_ms: u64,
    pub frame: AiFrameInfoV1,
    pub inference: AiInferenceInfoV1,
    pub object: AiObjectV1,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiGenerationEventDataV1 {
    pub schema_version: u32,
    pub source_id: String,
    pub media_generation: Option<String>,
    pub ai_generation: Option<String>,
    pub previous_media_generation: Option<String>,
    pub previous_ai_generation: Option<String>,
    pub state: String,
    pub reason: String,
    pub published_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AiStreamGapDataV1 {
    pub schema_version: u32,
    pub source_id: String,
    pub requested_event_id: Option<String>,
    pub earliest_event_id: Option<String>,
    pub latest_event_id: Option<String>,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AiCloudEvent {
    pub specversion: String,
    pub id: String,
    pub source: String,
    #[serde(rename = "type")]
    pub event_type: String,
    pub subject: String,
    pub time: String,
    pub datacontenttype: String,
    pub dataschema: String,
    pub data: Value,
}

impl AiCloudEvent {
    pub fn sequence(&self) -> Option<u64> {
        self.id.rsplit_once(':')?.1.parse().ok()
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct AiResultBusStatus {
    pub stream_id: String,
    pub latest_event_id: Option<String>,
    pub earliest_replay_event_id: Option<String>,
    pub published: u64,
    pub replay_depth: usize,
    pub replay_capacity: usize,
    pub lagged_events: u64,
}

#[derive(Debug, Clone)]
pub struct AiResultInput {
    pub source_id: String,
    pub media_generation: String,
    pub ai_generation: String,
    pub sequence: u64,
    pub pts_us: u64,
    pub published_at_ms: u64,
    pub frame: AiFrameInfoV1,
    pub inference: AiInferenceInfoV1,
    pub objects: Vec<AiObjectV1>,
    pub annotations: Vec<AiAnnotationV1>,
}

impl AiResultInput {
    pub fn data(&self) -> AiResultDataV1 {
        AiResultDataV1 {
            schema_version: 1,
            source_id: self.source_id.clone(),
            media_generation: self.media_generation.clone(),
            ai_generation: self.ai_generation.clone(),
            sequence: self.sequence,
            pts_us: self.pts_us,
            published_at_ms: self.published_at_ms,
            frame: self.frame.clone(),
            inference: self.inference.clone(),
            objects: self.objects.clone(),
            annotations: self.annotations.clone(),
        }
    }
}

#[derive(Debug)]
pub struct AiResultSubscription {
    pub receiver: broadcast::Receiver<Arc<AiCloudEvent>>,
    pub pending: VecDeque<Arc<AiCloudEvent>>,
    pub last_sequence: u64,
}

struct BusState {
    stream_id: String,
    next_sequence: u64,
    published: u64,
    lagged_events: u64,
    replay: VecDeque<Arc<AiCloudEvent>>,
    latest_frame: Option<Arc<AiCloudEvent>>,
}

#[derive(Clone)]
pub struct AiResultBus {
    source: Arc<StdMutex<String>>,
    schema_url: String,
    replay_capacity: usize,
    sender: broadcast::Sender<Arc<AiCloudEvent>>,
    state: Arc<StdMutex<BusState>>,
}

impl AiResultBus {
    pub fn new(source_id: String, replay_capacity: usize) -> Self {
        let capacity = replay_capacity.max(1);
        let (sender, _) = broadcast::channel(capacity.max(32));
        let stream_id = Uuid::new_v4().to_string();
        Self {
            source: Arc::new(StdMutex::new(format!("urn:aipc:source:{source_id}"))),
            schema_url: "/api/v1/ai/results/schema".into(),
            replay_capacity: capacity,
            sender,
            state: Arc::new(StdMutex::new(BusState {
                stream_id,
                next_sequence: 0,
                published: 0,
                lagged_events: 0,
                replay: VecDeque::with_capacity(capacity),
                latest_frame: None,
            })),
        }
    }

    pub fn set_source_id(&self, source_id: &str) {
        *self.source.lock().unwrap() = format!("urn:aipc:source:{source_id}");
    }

    pub fn publish<T: Serialize>(
        &self,
        event_type: &str,
        subject: String,
        data: &T,
        latest_frame: bool,
    ) -> Arc<AiCloudEvent> {
        let event = {
            let mut state = self.state.lock().unwrap();
            state.next_sequence += 1;
            state.published += 1;
            let id = format!("{}:{}", state.stream_id, state.next_sequence);
            let event = Arc::new(AiCloudEvent {
                specversion: "1.0".into(),
                id,
                source: self.source.lock().unwrap().clone(),
                event_type: event_type.into(),
                subject,
                time: utc_rfc3339(),
                datacontenttype: "application/json".into(),
                dataschema: self.schema_url.clone(),
                data: serde_json::to_value(data).unwrap_or_else(|_| json!({})),
            });
            if state.replay.len() == self.replay_capacity {
                state.replay.pop_front();
            }
            state.replay.push_back(event.clone());
            if latest_frame {
                state.latest_frame = Some(event.clone());
            }
            event
        };
        let _ = self.sender.send(event.clone());
        event
    }

    pub fn latest(&self) -> Option<Arc<AiCloudEvent>> {
        self.state.lock().unwrap().latest_frame.clone()
    }

    pub fn record_lagged(&self, skipped: u64) {
        let mut state = self.state.lock().unwrap();
        state.lagged_events = state.lagged_events.saturating_add(skipped);
    }

    pub fn status(&self) -> AiResultBusStatus {
        let state = self.state.lock().unwrap();
        AiResultBusStatus {
            stream_id: state.stream_id.clone(),
            latest_event_id: state.replay.back().map(|event| event.id.clone()),
            earliest_replay_event_id: state.replay.front().map(|event| event.id.clone()),
            published: state.published,
            replay_depth: state.replay.len(),
            replay_capacity: self.replay_capacity,
            lagged_events: state.lagged_events,
        }
    }

    pub fn subscribe_from(&self, cursor: Option<&str>) -> AiResultSubscription {
        let receiver = self.sender.subscribe();
        let (pending, last_sequence) = self.replay_after(cursor);
        AiResultSubscription {
            receiver,
            pending,
            last_sequence,
        }
    }

    pub fn replay_after_sequence(&self, sequence: u64) -> (VecDeque<Arc<AiCloudEvent>>, u64) {
        let (pending, cursor) = self.replay_after_sequence_inner(sequence, None);
        (pending, cursor)
    }

    fn replay_after(&self, cursor: Option<&str>) -> (VecDeque<Arc<AiCloudEvent>>, u64) {
        let Some(cursor) = cursor else {
            return (VecDeque::new(), 0);
        };
        let Some((stream, sequence)) = cursor.rsplit_once(':') else {
            return self.gap_for(Some(cursor));
        };
        let sequence = match sequence.parse::<u64>() {
            Ok(sequence) => sequence,
            Err(_) => return self.gap_for(Some(cursor)),
        };
        let expected_stream = self.state.lock().unwrap().stream_id.clone();
        if stream != expected_stream {
            return self.gap_for(Some(cursor));
        }
        self.replay_after_sequence_inner(sequence, Some(cursor))
    }

    fn replay_after_sequence_inner(
        &self,
        sequence: u64,
        requested: Option<&str>,
    ) -> (VecDeque<Arc<AiCloudEvent>>, u64) {
        let state = self.state.lock().unwrap();
        let earliest = state.replay.front().and_then(|event| event.sequence());
        let latest = state.replay.back().and_then(|event| event.sequence());
        let too_old = earliest.is_some_and(|earliest| sequence.saturating_add(1) < earliest);
        let too_new = latest.is_some_and(|latest| sequence > latest);
        if too_old || too_new {
            drop(state);
            return self.gap_for(requested);
        }
        let pending = state
            .replay
            .iter()
            .filter(|event| event.sequence().is_some_and(|item| item > sequence))
            .cloned()
            .collect();
        (pending, sequence)
    }

    fn gap_for(&self, requested: Option<&str>) -> (VecDeque<Arc<AiCloudEvent>>, u64) {
        let source = self.source.lock().unwrap().clone();
        let event = {
            let mut state = self.state.lock().unwrap();
            state.next_sequence += 1;
            let id = format!("{}:{}", state.stream_id, state.next_sequence);
            let earliest = state.replay.front().map(|item| item.id.clone());
            let latest = state.replay.back().map(|item| item.id.clone());
            Arc::new(AiCloudEvent {
                specversion: "1.0".into(),
                id,
                source: source.clone(),
                event_type: STREAM_GAP_TYPE.into(),
                subject: "stream".into(),
                time: utc_rfc3339(),
                datacontenttype: "application/json".into(),
                dataschema: self.schema_url.clone(),
                data: serde_json::to_value(AiStreamGapDataV1 {
                    schema_version: 1,
                    source_id: source.trim_start_matches("urn:aipc:source:").into(),
                    requested_event_id: requested.map(str::to_owned),
                    earliest_event_id: earliest,
                    latest_event_id: latest,
                    reason: "replay_cursor_out_of_range".into(),
                })
                .unwrap_or_else(|_| json!({})),
            })
        };
        let sequence = event.sequence().unwrap_or_default();
        (VecDeque::from([event]), sequence)
    }

    pub fn schema() -> &'static str {
        include_str!("ai_result_schema.json")
    }
}

#[derive(Debug, Clone)]
struct TrackState {
    object: AiObjectV1,
    context: TrackContext,
    seen_count: u32,
    confirmed: bool,
    last_seen_ms: u64,
    last_emitted_ms: u64,
    last_emitted_object: AiObjectV1,
}

#[derive(Debug, Clone)]
struct TrackContext {
    source_id: String,
    media_generation: String,
    ai_generation: String,
    sequence: u64,
    pts_us: u64,
    frame: AiFrameInfoV1,
    inference: AiInferenceInfoV1,
}

impl TrackContext {
    fn from_input(input: &AiResultInput) -> Self {
        Self {
            source_id: input.source_id.clone(),
            media_generation: input.media_generation.clone(),
            ai_generation: input.ai_generation.clone(),
            sequence: input.sequence,
            pts_us: input.pts_us,
            frame: input.frame.clone(),
            inference: input.inference.clone(),
        }
    }
}

#[derive(Debug, Default)]
pub struct AiLifecycleTracker {
    current_source_id: Option<String>,
    current_media_generation: Option<String>,
    current_ai_generation: Option<String>,
    tracks: HashMap<u64, TrackState>,
    confirmations: u32,
    lost_timeout: Duration,
    update_interval: Duration,
}

impl AiLifecycleTracker {
    pub fn new(confirmations: usize, lost_timeout_ms: u64, update_interval_ms: u64) -> Self {
        Self {
            confirmations: confirmations.max(1) as u32,
            lost_timeout: Duration::from_millis(lost_timeout_ms.max(1)),
            update_interval: Duration::from_millis(update_interval_ms.max(1)),
            ..Self::default()
        }
    }

    pub fn observe(&mut self, input: &AiResultInput) -> LifecycleBatch {
        let generation_changed = self.current_media_generation.as_deref()
            != Some(input.media_generation.as_str())
            || self.current_ai_generation.as_deref() != Some(input.ai_generation.as_str());
        let mut exited = Vec::new();
        let previous_media_generation = self.current_media_generation.clone();
        let previous_ai_generation = self.current_ai_generation.clone();
        if generation_changed {
            for state in self.tracks.values() {
                if state.confirmed {
                    exited.push(track_event_data(
                        &state.context,
                        state.object.clone(),
                        "generation_changed",
                        input.published_at_ms,
                    ));
                }
            }
            self.tracks.clear();
            self.current_source_id = Some(input.source_id.clone());
            self.current_media_generation = Some(input.media_generation.clone());
            self.current_ai_generation = Some(input.ai_generation.clone());
        }
        let mut present = HashSet::new();
        let context = TrackContext::from_input(input);
        let mut entered = Vec::new();
        let mut updated = Vec::new();
        for object in &input.objects {
            present.insert(object.track_id);
            let state = self
                .tracks
                .entry(object.track_id)
                .or_insert_with(|| TrackState {
                    object: object.clone(),
                    context: context.clone(),
                    seen_count: 0,
                    confirmed: false,
                    last_seen_ms: input.published_at_ms,
                    last_emitted_ms: 0,
                    last_emitted_object: object.clone(),
                });
            state.object = object.clone();
            state.context = context.clone();
            state.seen_count = state.seen_count.saturating_add(1);
            state.last_seen_ms = input.published_at_ms;
            if !state.confirmed && state.seen_count >= self.confirmations {
                state.confirmed = true;
                state.last_emitted_ms = input.published_at_ms;
                state.last_emitted_object = object.clone();
                entered.push(track_event_data(
                    &context,
                    object.clone(),
                    "confirmed",
                    input.published_at_ms,
                ));
            } else if state.confirmed
                && let Some(reason) =
                    update_reason(state, object, input.published_at_ms, self.update_interval)
            {
                state.last_emitted_ms = input.published_at_ms;
                state.last_emitted_object = object.clone();
                updated.push(track_event_data(
                    &context,
                    object.clone(),
                    reason,
                    input.published_at_ms,
                ));
            }
        }
        exited.extend(self.expire_except(input.published_at_ms, &present));
        LifecycleBatch {
            generation: if generation_changed {
                Some(AiGenerationEventDataV1 {
                    schema_version: 1,
                    source_id: input.source_id.clone(),
                    media_generation: Some(input.media_generation.clone()),
                    ai_generation: Some(input.ai_generation.clone()),
                    previous_media_generation,
                    previous_ai_generation,
                    state: "started".into(),
                    reason: "generation_changed".into(),
                    published_at_ms: input.published_at_ms,
                })
            } else {
                None
            },
            exited,
            entered,
            updated,
        }
    }

    pub fn finish(&mut self, reason: &str, published_at_ms: u64) -> LifecycleFinish {
        let mut exited = Vec::new();
        for state in self.tracks.values() {
            if state.confirmed {
                exited.push(track_event_data(
                    &state.context,
                    state.object.clone(),
                    reason,
                    published_at_ms,
                ));
            }
        }
        let generation = self
            .current_ai_generation
            .as_ref()
            .map(|_| AiGenerationEventDataV1 {
                schema_version: 1,
                source_id: self
                    .current_source_id
                    .clone()
                    .unwrap_or_else(|| "camera0".into()),
                media_generation: None,
                ai_generation: None,
                previous_media_generation: self.current_media_generation.clone(),
                previous_ai_generation: self.current_ai_generation.clone(),
                state: "stopped".into(),
                reason: reason.into(),
                published_at_ms,
            });
        self.tracks.clear();
        self.current_source_id = None;
        self.current_media_generation = None;
        self.current_ai_generation = None;
        LifecycleFinish { generation, exited }
    }

    pub fn expire(&mut self, now_ms: u64) -> Vec<AiTrackEventDataV1> {
        self.expire_except(now_ms, &HashSet::new())
    }

    fn expire_except(&mut self, now_ms: u64, present: &HashSet<u64>) -> Vec<AiTrackEventDataV1> {
        let stale: Vec<u64> = self
            .tracks
            .iter()
            .filter(|(track_id, state)| {
                !present.contains(track_id)
                    && now_ms.saturating_sub(state.last_seen_ms)
                        >= self.lost_timeout.as_millis() as u64
            })
            .map(|(track_id, _)| *track_id)
            .collect();
        let mut exited = Vec::new();
        for track_id in stale {
            if let Some(state) = self.tracks.remove(&track_id)
                && state.confirmed
            {
                exited.push(track_event_data(
                    &state.context,
                    state.object,
                    "lost_timeout",
                    now_ms,
                ));
            }
        }
        exited
    }
}

#[derive(Debug, Default)]
pub struct LifecycleBatch {
    pub generation: Option<AiGenerationEventDataV1>,
    pub exited: Vec<AiTrackEventDataV1>,
    pub entered: Vec<AiTrackEventDataV1>,
    pub updated: Vec<AiTrackEventDataV1>,
}

#[derive(Debug, Default)]
pub struct LifecycleFinish {
    pub generation: Option<AiGenerationEventDataV1>,
    pub exited: Vec<AiTrackEventDataV1>,
}

fn track_event_data(
    context: &TrackContext,
    object: AiObjectV1,
    reason: &str,
    published_at_ms: u64,
) -> AiTrackEventDataV1 {
    AiTrackEventDataV1 {
        schema_version: 1,
        source_id: context.source_id.clone(),
        media_generation: context.media_generation.clone(),
        ai_generation: context.ai_generation.clone(),
        sequence: context.sequence,
        pts_us: context.pts_us,
        published_at_ms,
        frame: context.frame.clone(),
        inference: context.inference.clone(),
        object,
        reason: reason.into(),
    }
}

fn update_reason(
    state: &TrackState,
    current: &AiObjectV1,
    now_ms: u64,
    interval: Duration,
) -> Option<&'static str> {
    let elapsed = now_ms.saturating_sub(state.last_emitted_ms);
    let significant = iou(&state.last_emitted_object.bbox, &current.bbox) < 0.85
        || (state.last_emitted_object.confidence - current.confidence).abs() >= 0.1
        || state.last_emitted_object.class_id != current.class_id
        || state.last_emitted_object.label != current.label;
    if significant {
        Some("changed")
    } else if elapsed >= interval.as_millis() as u64 {
        Some("heartbeat")
    } else {
        None
    }
}

fn iou(left: &AiBoundingBoxV1, right: &AiBoundingBoxV1) -> f64 {
    let x1 = left.x.max(right.x);
    let y1 = left.y.max(right.y);
    let x2 = (left.x + left.width).min(right.x + right.width);
    let y2 = (left.y + left.height).min(right.y + right.height);
    let intersection = (x2 - x1).max(0.0) * (y2 - y1).max(0.0);
    let union = left.width * left.height + right.width * right.height - intersection;
    if union <= 0.0 {
        0.0
    } else {
        intersection / union
    }
}

fn utc_rfc3339() -> String {
    OffsetDateTime::now_utc()
        .format(&Rfc3339)
        .unwrap_or_else(|_| "1970-01-01T00:00:00Z".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input(sequence: u64, pts_us: u64, x: f64) -> AiResultInput {
        AiResultInput {
            source_id: "camera0".into(),
            media_generation: "media-1".into(),
            ai_generation: "ai-1".into(),
            sequence,
            pts_us,
            published_at_ms: sequence * 100,
            frame: AiFrameInfoV1 {
                width: 1920,
                height: 1080,
                coordinate_space: "main_normalized_top_left".into(),
            },
            inference: AiInferenceInfoV1 {
                project: "test".into(),
                algorithm: "yolov5".into(),
                model: "model.rknn".into(),
                duration_us: 10,
            },
            objects: vec![AiObjectV1 {
                track_id: 1,
                class_id: 0,
                label: "person".into(),
                confidence: 0.9,
                bbox: AiBoundingBoxV1 {
                    x,
                    y: 0.2,
                    width: 0.2,
                    height: 0.3,
                },
            }],
            annotations: vec![],
        }
    }

    #[test]
    fn cloud_event_has_stable_required_fields() {
        let bus = AiResultBus::new("camera0".into(), 4);
        let event = bus.publish(
            FRAME_RESULT_TYPE,
            "frame/ai-1/1".into(),
            &input(1, 1, 0.1).data(),
            true,
        );
        assert_eq!(event.specversion, "1.0");
        assert!(event.id.ends_with(":1"));
        assert_eq!(event.event_type, FRAME_RESULT_TYPE);
        assert_eq!(
            event.data["frame"]["coordinate_space"],
            "main_normalized_top_left"
        );
    }

    #[test]
    fn replay_reports_gap_for_expired_cursor() {
        let bus = AiResultBus::new("camera0".into(), 2);
        let first = bus.publish(
            FRAME_RESULT_TYPE,
            "frame/1".into(),
            &input(1, 1, 0.1).data(),
            true,
        );
        bus.publish(
            FRAME_RESULT_TYPE,
            "frame/2".into(),
            &input(2, 2, 0.1).data(),
            true,
        );
        bus.publish(
            FRAME_RESULT_TYPE,
            "frame/3".into(),
            &input(3, 3, 0.1).data(),
            true,
        );
        bus.publish(
            FRAME_RESULT_TYPE,
            "frame/4".into(),
            &input(4, 4, 0.1).data(),
            true,
        );
        let subscription = bus.subscribe_from(Some(&first.id));
        assert_eq!(subscription.pending[0].event_type, STREAM_GAP_TYPE);
    }

    #[test]
    fn lifecycle_requires_confirmation_and_emits_exit() {
        let mut tracker = AiLifecycleTracker::new(2, 500, 500);
        assert!(tracker.observe(&input(1, 1, 0.1)).entered.is_empty());
        assert_eq!(tracker.observe(&input(2, 2, 0.1)).entered.len(), 1);
        let mut missing = input(8, 8, 0.1);
        missing.objects.clear();
        assert_eq!(tracker.observe(&missing).exited.len(), 1);
    }

    #[test]
    fn slow_subscriber_never_blocks_result_publish() {
        let bus = AiResultBus::new("camera0".into(), 4);
        let _subscriber = bus.subscribe_from(None);
        for sequence in 1..=1000 {
            bus.publish(
                FRAME_RESULT_TYPE,
                format!("frame/{sequence}"),
                &input(sequence, sequence, 0.1).data(),
                true,
            );
        }
        let status = bus.status();
        assert_eq!(status.published, 1000);
        assert_eq!(status.replay_depth, 4);
        assert!(status.latest_event_id.unwrap().ends_with(":1000"));
    }

    #[test]
    fn generation_change_exits_confirmed_tracks() {
        let mut tracker = AiLifecycleTracker::new(1, 500, 500);
        assert_eq!(tracker.observe(&input(1, 1, 0.1)).entered.len(), 1);
        let mut next = input(2, 2, 0.1);
        next.ai_generation = "ai-2".into();
        let batch = tracker.observe(&next);
        assert_eq!(batch.exited.len(), 1);
        assert_eq!(batch.exited[0].reason, "generation_changed");
        assert_eq!(
            batch.generation.unwrap().ai_generation.as_deref(),
            Some("ai-2")
        );
    }

    #[test]
    fn lifecycle_expires_without_another_inference_result() {
        let mut tracker = AiLifecycleTracker::new(1, 500, 500);
        assert_eq!(tracker.observe(&input(1, 1, 0.1)).entered.len(), 1);
        assert!(tracker.expire(599).is_empty());
        let exited = tracker.expire(600);
        assert_eq!(exited.len(), 1);
        assert_eq!(exited[0].published_at_ms, 600);
    }

    #[test]
    fn generation_exit_uses_transition_publish_time() {
        let mut tracker = AiLifecycleTracker::new(1, 500, 500);
        tracker.observe(&input(1, 1, 0.1));
        let mut next = input(9, 9, 0.1);
        next.ai_generation = "ai-2".into();
        let batch = tracker.observe(&next);
        assert_eq!(batch.exited[0].published_at_ms, 900);
    }

    #[test]
    fn bundled_schema_is_valid_json() {
        let schema: Value = serde_json::from_str(AiResultBus::schema()).unwrap();
        assert_eq!(schema["$id"], "/api/v1/ai/results/schema");
        assert!(schema["$defs"]["frameResult"].is_object());
    }
}
