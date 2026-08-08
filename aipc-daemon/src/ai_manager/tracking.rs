use super::{AiDetection, AiFrame, RenderedRegions, TimedMetadata};
use crate::ai::AiFitMode;
use serde_json::Value;
use std::collections::VecDeque;
use std::time::{Duration, Instant};

pub(super) struct Tracker {
    next_id: u64,
    previous: Vec<(u64, UntrackedDetection, u64)>,
    retention_us: u64,
    media_generation: Option<String>,
}

#[derive(Clone)]
pub(super) struct UntrackedDetection {
    pub(super) class_id: i64,
    pub(super) label: String,
    pub(super) confidence: f64,
    pub(super) x: f64,
    pub(super) y: f64,
    pub(super) width: f64,
    pub(super) height: f64,
}

impl Tracker {
    pub(super) fn new(retention_us: u64) -> Self {
        Self {
            next_id: 0,
            previous: Vec::new(),
            retention_us,
            media_generation: None,
        }
    }

    pub(super) fn update(
        &mut self,
        current: Vec<UntrackedDetection>,
        pts: u64,
        media_generation: &str,
    ) -> Vec<AiDetection> {
        if self.media_generation.as_deref() != Some(media_generation) {
            self.previous.clear();
            self.media_generation = Some(media_generation.into());
        }
        let mut used = vec![false; self.previous.len()];
        let mut output = Vec::new();
        let mut next_previous = Vec::new();
        for item in current {
            let mut best = None;
            for (index, (_, previous, _)) in self.previous.iter().enumerate() {
                if used[index] || previous.class_id != item.class_id {
                    continue;
                }
                let score = iou(previous, &item);
                if score >= 0.3 && best.is_none_or(|(_, best_score)| score > best_score) {
                    best = Some((index, score));
                }
            }
            let track_id = if let Some((index, _)) = best {
                used[index] = true;
                self.previous[index].0
            } else {
                self.next_id += 1;
                self.next_id
            };
            output.push(AiDetection {
                track_id,
                class_id: item.class_id,
                label: item.label.clone(),
                confidence: item.confidence,
                x: item.x,
                y: item.y,
                width: item.width,
                height: item.height,
            });
            next_previous.push((track_id, item, pts));
        }
        for (index, previous) in self.previous.iter().enumerate() {
            if !used[index] && pts.saturating_sub(previous.2) <= self.retention_us {
                next_previous.push(previous.clone());
            }
        }
        self.previous = next_previous;
        output
    }
}

pub(super) fn render_regions(
    history: &VecDeque<TimedMetadata>,
    now: Instant,
    ttl: Duration,
) -> Option<RenderedRegions> {
    let latest = history.back()?;
    if now.saturating_duration_since(latest.received_at) > ttl {
        return None;
    }
    let previous = history
        .get(history.len().saturating_sub(2))
        .filter(|item| item.metadata.generation == latest.metadata.generation);
    let sample_at = now.checked_sub(Duration::from_millis(100)).unwrap_or(now);
    let regions = latest
        .metadata
        .detections
        .iter()
        .map(|current| {
            let Some((previous, previous_at)) = previous.and_then(|snapshot| {
                snapshot
                    .metadata
                    .detections
                    .iter()
                    .find(|item| item.track_id == current.track_id)
                    .map(|item| (item, snapshot.received_at))
            }) else {
                return current.clone();
            };
            let interval = latest.received_at.saturating_duration_since(previous_at);
            if interval.is_zero() {
                return current.clone();
            }
            let factor = if sample_at <= previous_at {
                0.0
            } else if sample_at <= latest.received_at {
                sample_at
                    .saturating_duration_since(previous_at)
                    .as_secs_f64()
                    / interval.as_secs_f64()
            } else {
                let extrapolation = sample_at
                    .saturating_duration_since(latest.received_at)
                    .min(Duration::from_millis(150));
                1.0 + (extrapolation.as_secs_f64() / interval.as_secs_f64()).min(1.0)
            };
            interpolate_detection(previous, current, factor)
        })
        .collect();
    Some(RenderedRegions {
        generation: latest.metadata.generation.clone(),
        main_width: latest.metadata.main_width,
        main_height: latest.metadata.main_height,
        regions,
    })
}

fn interpolate_detection(
    previous: &AiDetection,
    current: &AiDetection,
    factor: f64,
) -> AiDetection {
    let lerp = |from: f64, to: f64| from + (to - from) * factor;
    let x = lerp(previous.x, current.x).clamp(0.0, 1.0);
    let y = lerp(previous.y, current.y).clamp(0.0, 1.0);
    AiDetection {
        track_id: current.track_id,
        class_id: current.class_id,
        label: current.label.clone(),
        confidence: current.confidence,
        x,
        y,
        width: lerp(previous.width, current.width).clamp(0.0, 1.0 - x),
        height: lerp(previous.height, current.height).clamp(0.0, 1.0 - y),
    }
}

fn iou(left: &UntrackedDetection, right: &UntrackedDetection) -> f64 {
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

pub(super) fn map_point(frame: &AiFrame, x: f64, y: f64) -> (f64, f64) {
    let (source_x, source_y) = match frame.fit_mode {
        AiFitMode::Stretch => (
            x / frame.width as f64 * frame.main_width as f64,
            y / frame.height as f64 * frame.main_height as f64,
        ),
        AiFitMode::Contain => {
            let content_width =
                frame.width as i32 - frame.transform.pad_left - frame.transform.pad_right;
            let content_height =
                frame.height as i32 - frame.transform.pad_top - frame.transform.pad_bottom;
            (
                (x - frame.transform.pad_left as f64) / content_width.max(1) as f64
                    * frame.main_width as f64,
                (y - frame.transform.pad_top as f64) / content_height.max(1) as f64
                    * frame.main_height as f64,
            )
        }
        AiFitMode::Cover => (
            frame.transform.crop_x as f64
                + x / frame.width as f64 * frame.transform.crop_width as f64,
            frame.transform.crop_y as f64
                + y / frame.height as f64 * frame.transform.crop_height as f64,
        ),
    };
    (
        (source_x / frame.main_width.max(1) as f64).clamp(0.0, 1.0),
        (source_y / frame.main_height.max(1) as f64).clamp(0.0, 1.0),
    )
}

pub(super) fn number(value: &Value, key: &str) -> anyhow::Result<f64> {
    value
        .get(key)
        .and_then(Value::as_f64)
        .ok_or_else(|| anyhow::anyhow!("detection field {key} is missing"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn detection(x: f64) -> UntrackedDetection {
        UntrackedDetection {
            class_id: 0,
            label: "person".into(),
            confidence: 0.9,
            x,
            y: 0.1,
            width: 0.2,
            height: 0.3,
        }
    }

    #[test]
    fn media_generation_change_starts_new_track_ids() {
        let mut tracker = Tracker::new(500_000);
        let first = tracker.update(vec![detection(0.1)], 100, "media-1");
        let same = tracker.update(vec![detection(0.11)], 200, "media-1");
        let restarted = tracker.update(vec![detection(0.11)], 10, "media-2");
        assert_eq!(first[0].track_id, same[0].track_id);
        assert_ne!(first[0].track_id, restarted[0].track_id);
    }
}
