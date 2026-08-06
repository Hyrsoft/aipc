use crate::config::WorkerConfig;
use crate::preview::PreviewHub;
use crate::recording::{RecordingManager, RecordingSettingsUpdate};
use crate::rtsp::RtspServer;
use crate::supervisor::{ActionAccepted, SupervisorError, SupervisorHandle};
use crate::webrtc::{WebRtcError, WebRtcServer};
use axum::Json;
use axum::Router;
use axum::body::Body;
use axum::extract::{ConnectInfo, Path as AxumPath, Query, State, WebSocketUpgrade};
use axum::http::{HeaderMap, StatusCode, header};
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use futures_util::stream;
use serde::Deserialize;
use serde_json::json;
use std::convert::Infallible;
use std::net::SocketAddr;
use std::path::Path;
use std::path::PathBuf;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncSeekExt, AsyncWrite, AsyncWriteExt, SeekFrom};
use tokio_util::io::ReaderStream;
use tower_http::services::{ServeDir, ServeFile};

#[derive(Clone)]
struct AppState {
    supervisor: SupervisorHandle,
    preview: PreviewHub,
    recording: RecordingManager,
    rtsp: RtspServer,
    webrtc: WebRtcServer,
}

pub fn router(
    supervisor: SupervisorHandle,
    recording: RecordingManager,
    rtsp: RtspServer,
    webrtc: WebRtcServer,
    web_dir: &Path,
) -> Router {
    let index = web_dir.join("index.html");
    let static_service = ServeDir::new(web_dir).fallback(ServeFile::new(index));
    Router::new()
        .route("/healthz", get(healthz))
        .route("/api/v1/status", get(status))
        .route("/api/v1/config", get(config).put(apply_config))
        .route("/api/v1/worker/start", post(start))
        .route("/api/v1/worker/stop", post(stop))
        .route("/api/v1/worker/restart", post(restart))
        .route("/api/v1/events", get(events))
        .route("/api/v1/logs", get(logs))
        .route("/api/v1/preview/status", get(preview_status))
        .route("/api/v1/preview/ws", get(preview_ws))
        .route(
            "/api/v1/recording/settings",
            get(recording_settings).put(update_recording_settings),
        )
        .route("/api/v1/recording/status", get(recording_status))
        .route("/api/v1/recording/start", post(recording_start))
        .route("/api/v1/recording/stop", post(recording_stop))
        .route("/api/v1/recordings", get(recordings))
        .route(
            "/api/v1/recordings/{id}/content",
            get(recording_content).head(recording_head),
        )
        .route(
            "/api/v1/recordings/{id}/audio",
            get(recording_audio).head(recording_audio_head),
        )
        .route("/api/v1/recordings/{id}/download", get(recording_download))
        .route("/api/v1/recordings/export", post(recordings_export))
        .route("/api/v1/recordings/delete", post(recordings_delete))
        .route("/api/v1/rtsp/status", get(rtsp_status))
        .route("/api/v1/webrtc/status", get(webrtc_status))
        .route("/api/v1/webrtc/sessions", post(webrtc_create))
        .route(
            "/api/v1/webrtc/sessions/{id}",
            axum::routing::delete(webrtc_delete),
        )
        .fallback_service(static_service)
        .with_state(AppState {
            preview: supervisor.preview.clone(),
            recording,
            rtsp,
            webrtc,
            supervisor,
        })
}

#[derive(Deserialize)]
struct WebRtcOffer {
    r#type: String,
    sdp: String,
}

async fn webrtc_status(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.webrtc.status())
}

async fn webrtc_create(
    State(state): State<AppState>,
    ConnectInfo(remote): ConnectInfo<SocketAddr>,
    Json(offer): Json<WebRtcOffer>,
) -> Result<impl IntoResponse, WebRtcApiError> {
    if offer.r#type != "offer" || offer.sdp.trim().is_empty() {
        return Err(WebRtcApiError(WebRtcError::InvalidOffer(
            "body must contain type=offer and a non-empty SDP".into(),
        )));
    }
    let answer = state.webrtc.create_session(offer.sdp, remote).await?;
    Ok((StatusCode::CREATED, Json(answer)))
}

async fn webrtc_delete(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<StatusCode, WebRtcApiError> {
    state.webrtc.delete_session(&id).await?;
    Ok(StatusCode::NO_CONTENT)
}

async fn recording_settings(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.recording.settings().await)
}

async fn update_recording_settings(
    State(state): State<AppState>,
    Json(update): Json<RecordingSettingsUpdate>,
) -> Result<impl IntoResponse, AppError> {
    Ok(Json(state.recording.update_settings(update).await?))
}

async fn recording_status(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.recording.status().await)
}

async fn recording_start(State(state): State<AppState>) -> Result<impl IntoResponse, AppError> {
    Ok((StatusCode::ACCEPTED, Json(state.recording.start().await?)))
}

async fn recording_stop(State(state): State<AppState>) -> Result<impl IntoResponse, AppError> {
    Ok((StatusCode::ACCEPTED, Json(state.recording.stop().await?)))
}

#[derive(Deserialize)]
struct RecordingsQuery {
    offset: Option<usize>,
    limit: Option<usize>,
}

async fn recordings(
    State(state): State<AppState>,
    Query(query): Query<RecordingsQuery>,
) -> impl IntoResponse {
    Json(
        state
            .recording
            .list(query.offset.unwrap_or(0), query.limit.unwrap_or(25))
            .await,
    )
}

async fn recording_content(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    headers: HeaderMap,
) -> Result<Response, AppError> {
    serve_recording(&state.recording, &id, &headers, false, false).await
}

async fn recording_head(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    headers: HeaderMap,
) -> Result<Response, AppError> {
    serve_recording(&state.recording, &id, &headers, false, true).await
}

async fn recording_download(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    headers: HeaderMap,
) -> Result<Response, AppError> {
    serve_recording(&state.recording, &id, &headers, true, false).await
}

async fn recording_audio(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    headers: HeaderMap,
) -> Result<Response, AppError> {
    serve_recording_audio(&state.recording, &id, &headers, false).await
}

async fn recording_audio_head(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    headers: HeaderMap,
) -> Result<Response, AppError> {
    serve_recording_audio(&state.recording, &id, &headers, true).await
}

async fn serve_recording_audio(
    manager: &RecordingManager,
    id: &str,
    headers: &HeaderMap,
    head_only: bool,
) -> Result<Response, AppError> {
    let (entry, path) = manager.audio_path_for(id).await?;
    serve_media_file(
        entry.id,
        entry.created_at_ms,
        entry
            .audio_file_name
            .unwrap_or_else(|| "recording.wav".into()),
        path,
        headers,
        "audio/wav",
        "inline",
        head_only,
    )
    .await
}

async fn serve_recording(
    manager: &RecordingManager,
    id: &str,
    headers: &HeaderMap,
    download: bool,
    head_only: bool,
) -> Result<Response, AppError> {
    let (entry, path) = manager.path_for(id).await?;
    serve_media_file(
        entry.id,
        entry.created_at_ms,
        entry.file_name,
        path,
        headers,
        "video/mp4",
        if download { "attachment" } else { "inline" },
        head_only,
    )
    .await
}

async fn serve_media_file(
    id: String,
    created_at_ms: u64,
    file_name: String,
    path: PathBuf,
    headers: &HeaderMap,
    content_type: &'static str,
    disposition: &'static str,
    head_only: bool,
) -> Result<Response, AppError> {
    let metadata = tokio::fs::metadata(&path).await?;
    let size = metadata.len();
    let range = headers
        .get(header::RANGE)
        .and_then(|value| value.to_str().ok());
    let (status, start, end) = match range {
        Some(value) => match parse_range(value, size) {
            Some((start, end)) => (StatusCode::PARTIAL_CONTENT, start, end),
            None => {
                return Ok(Response::builder()
                    .status(StatusCode::RANGE_NOT_SATISFIABLE)
                    .header(header::CONTENT_RANGE, format!("bytes */{size}"))
                    .body(Body::empty())?);
            }
        },
        None => (StatusCode::OK, 0, size.saturating_sub(1)),
    };
    let length = if size == 0 { 0 } else { end - start + 1 };
    let mut builder = Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, content_type)
        .header(header::ACCEPT_RANGES, "bytes")
        .header(header::CONTENT_LENGTH, length)
        .header(
            header::ETAG,
            format!("\"{}-{}-{}\"", id, size, created_at_ms),
        )
        .header(
            header::CONTENT_DISPOSITION,
            format!("{disposition}; filename=\"{}\"", file_name),
        );
    if status == StatusCode::PARTIAL_CONTENT {
        builder = builder.header(header::CONTENT_RANGE, format!("bytes {start}-{end}/{size}"));
    }
    if head_only || length == 0 {
        return Ok(builder.body(Body::empty())?);
    }
    let mut file = tokio::fs::File::open(path).await?;
    file.seek(SeekFrom::Start(start)).await?;
    let stream = ReaderStream::new(file.take(length));
    Ok(builder.body(Body::from_stream(stream))?)
}

fn parse_range(value: &str, size: u64) -> Option<(u64, u64)> {
    let value = value.strip_prefix("bytes=")?;
    if value.contains(',') || size == 0 {
        return None;
    }
    let (start, end) = value.split_once('-')?;
    if start.is_empty() {
        let suffix: u64 = end.parse().ok()?;
        if suffix == 0 {
            return None;
        }
        return Some((size.saturating_sub(suffix.min(size)), size - 1));
    }
    let start: u64 = start.parse().ok()?;
    if start >= size {
        return None;
    }
    let end = if end.is_empty() {
        size - 1
    } else {
        end.parse::<u64>().ok()?.min(size - 1)
    };
    (end >= start).then_some((start, end))
}

#[derive(Deserialize)]
struct IdSelection {
    ids: Vec<String>,
}

async fn recordings_delete(
    State(state): State<AppState>,
    Json(selection): Json<IdSelection>,
) -> Result<impl IntoResponse, AppError> {
    let count = state.recording.delete(&selection.ids).await?;
    Ok(Json(json!({"deleted": count})))
}

async fn recordings_export(
    State(state): State<AppState>,
    Json(selection): Json<IdSelection>,
) -> Result<Response, AppError> {
    let settings = state.recording.settings().await;
    if selection.ids.is_empty() || selection.ids.len() > settings.max_export_files {
        return Err(AppError::bad_request("invalid recording selection"));
    }
    let mut files = Vec::new();
    for id in selection.ids {
        let (entry, path) = state.recording.path_for(&id).await?;
        files.push((entry.file_name.clone(), path));
        if entry.audio_available {
            if let Ok((audio_entry, audio_path)) = state.recording.audio_path_for(&id).await {
                if let Some(name) = audio_entry.audio_file_name {
                    files.push((name, audio_path));
                }
            }
        }
    }
    let (reader, writer) = tokio::io::duplex(128 * 1024);
    tokio::spawn(async move {
        let _ = write_zip64(writer, files).await;
    });
    let file_name = format!("aipc-recordings-{}.zip", crate::model::now_ms());
    Ok(Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "application/zip")
        .header(
            header::CONTENT_DISPOSITION,
            format!("attachment; filename=\"{file_name}\""),
        )
        .body(Body::from_stream(ReaderStream::new(reader)))?)
}

struct ZipEntry {
    name: Vec<u8>,
    crc: u32,
    size: u64,
    offset: u64,
}

async fn write_zip64<W: AsyncWrite + Unpin>(
    mut writer: W,
    files: Vec<(String, PathBuf)>,
) -> anyhow::Result<()> {
    let mut entries = Vec::new();
    let mut offset = 0_u64;
    for (name, path) in files {
        let safe_name: Vec<u8> = name
            .bytes()
            .map(|byte| {
                if matches!(byte, b'/' | b'\\') {
                    b'_'
                } else {
                    byte
                }
            })
            .collect();
        let local_offset = offset;
        let mut header = Vec::new();
        push_u32(&mut header, 0x04034b50);
        push_u16(&mut header, 45);
        push_u16(&mut header, 0x0008);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u32(&mut header, 0);
        push_u32(&mut header, u32::MAX);
        push_u32(&mut header, u32::MAX);
        push_u16(&mut header, safe_name.len() as u16);
        push_u16(&mut header, 20);
        header.extend_from_slice(&safe_name);
        push_u16(&mut header, 0x0001);
        push_u16(&mut header, 16);
        push_u64(&mut header, 0);
        push_u64(&mut header, 0);
        writer.write_all(&header).await?;
        offset += header.len() as u64;
        let mut file = tokio::fs::File::open(path).await?;
        let mut buffer = vec![0_u8; 64 * 1024];
        let mut hasher = crc32fast::Hasher::new();
        let mut size = 0_u64;
        loop {
            let count = file.read(&mut buffer).await?;
            if count == 0 {
                break;
            }
            hasher.update(&buffer[..count]);
            writer.write_all(&buffer[..count]).await?;
            size += count as u64;
            offset += count as u64;
        }
        let crc = hasher.finalize();
        let mut descriptor = Vec::new();
        push_u32(&mut descriptor, 0x08074b50);
        push_u32(&mut descriptor, crc);
        push_u64(&mut descriptor, size);
        push_u64(&mut descriptor, size);
        writer.write_all(&descriptor).await?;
        offset += descriptor.len() as u64;
        entries.push(ZipEntry {
            name: safe_name,
            crc,
            size,
            offset: local_offset,
        });
    }
    let central_offset = offset;
    for entry in &entries {
        let mut header = Vec::new();
        push_u32(&mut header, 0x02014b50);
        push_u16(&mut header, 45);
        push_u16(&mut header, 45);
        push_u16(&mut header, 0x0008);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u32(&mut header, entry.crc);
        push_u32(&mut header, u32::MAX);
        push_u32(&mut header, u32::MAX);
        push_u16(&mut header, entry.name.len() as u16);
        push_u16(&mut header, 28);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u32(&mut header, 0);
        push_u32(&mut header, u32::MAX);
        header.extend_from_slice(&entry.name);
        push_u16(&mut header, 0x0001);
        push_u16(&mut header, 24);
        push_u64(&mut header, entry.size);
        push_u64(&mut header, entry.size);
        push_u64(&mut header, entry.offset);
        writer.write_all(&header).await?;
        offset += header.len() as u64;
    }
    let central_size = offset - central_offset;
    let zip64_offset = offset;
    let mut ending = Vec::new();
    push_u32(&mut ending, 0x06064b50);
    push_u64(&mut ending, 44);
    push_u16(&mut ending, 45);
    push_u16(&mut ending, 45);
    push_u32(&mut ending, 0);
    push_u32(&mut ending, 0);
    push_u64(&mut ending, entries.len() as u64);
    push_u64(&mut ending, entries.len() as u64);
    push_u64(&mut ending, central_size);
    push_u64(&mut ending, central_offset);
    push_u32(&mut ending, 0x07064b50);
    push_u32(&mut ending, 0);
    push_u64(&mut ending, zip64_offset);
    push_u32(&mut ending, 1);
    push_u32(&mut ending, 0x06054b50);
    push_u16(&mut ending, 0);
    push_u16(&mut ending, 0);
    push_u16(&mut ending, u16::MAX);
    push_u16(&mut ending, u16::MAX);
    push_u32(&mut ending, u32::MAX);
    push_u32(&mut ending, u32::MAX);
    push_u16(&mut ending, 0);
    writer.write_all(&ending).await?;
    writer.shutdown().await?;
    Ok(())
}

fn push_u16(output: &mut Vec<u8>, value: u16) {
    output.extend_from_slice(&value.to_le_bytes());
}
fn push_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}
fn push_u64(output: &mut Vec<u8>, value: u64) {
    output.extend_from_slice(&value.to_le_bytes());
}

async fn rtsp_status(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.rtsp.status())
}

struct AppError {
    status: StatusCode,
    message: String,
}

impl AppError {
    fn bad_request(message: impl Into<String>) -> Self {
        Self {
            status: StatusCode::BAD_REQUEST,
            message: message.into(),
        }
    }
}

impl<E: Into<anyhow::Error>> From<E> for AppError {
    fn from(error: E) -> Self {
        let message = error.into().to_string();
        let status = if message.contains("not found") {
            StatusCode::NOT_FOUND
        } else if message.contains("already")
            || message.contains("while recording")
            || message.contains("not active")
        {
            StatusCode::CONFLICT
        } else if message.contains("not ready") {
            StatusCode::SERVICE_UNAVAILABLE
        } else if message.contains("invalid")
            || message.contains("must be")
            || message.contains("outside")
            || message.contains("insufficient")
            || message.contains("disabled")
        {
            StatusCode::BAD_REQUEST
        } else {
            StatusCode::INTERNAL_SERVER_ERROR
        };
        Self { status, message }
    }
}

impl IntoResponse for AppError {
    fn into_response(self) -> Response {
        (
            self.status,
            Json(json!({"error": {"code": "recording_error", "message": self.message}})),
        )
            .into_response()
    }
}

async fn preview_status(State(state): State<AppState>) -> impl IntoResponse {
    let status = state.preview.status();
    let mut value = serde_json::to_value(&status).unwrap_or_else(|_| json!({}));
    if let Some(root) = value.as_object_mut() {
        let mut video = root.clone();
        video.remove("audio");
        root.insert("video".into(), serde_json::Value::Object(video));
    }
    Json(value)
}

async fn preview_ws(State(state): State<AppState>, upgrade: WebSocketUpgrade) -> Response {
    if !state.preview.enabled() {
        return (StatusCode::SERVICE_UNAVAILABLE, "preview disabled").into_response();
    }
    let Some(guard) = state.preview.acquire_client() else {
        return (
            StatusCode::TOO_MANY_REQUESTS,
            "preview client limit reached",
        )
            .into_response();
    };
    let preview = state.preview.clone();
    upgrade
        .on_upgrade(move |socket| async move { preview.serve_socket(socket, guard).await })
        .into_response()
}

async fn healthz(State(state): State<AppState>) -> impl IntoResponse {
    let status = state.supervisor.status.borrow().clone();
    Json(json!({"ok": true, "state": status.state}))
}

async fn status(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.supervisor.status.borrow().clone())
}

async fn config(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.supervisor.persistent.read().await.clone())
}

async fn apply_config(
    State(state): State<AppState>,
    Json(config): Json<WorkerConfig>,
) -> Result<impl IntoResponse, ApiError> {
    let accepted = state.supervisor.apply(config).await?;
    Ok((StatusCode::ACCEPTED, Json(accepted)))
}

async fn start(State(state): State<AppState>) -> Result<impl IntoResponse, ApiError> {
    accepted(state.supervisor.start().await)
}

async fn stop(State(state): State<AppState>) -> Result<impl IntoResponse, ApiError> {
    accepted(state.supervisor.stop().await)
}

async fn restart(State(state): State<AppState>) -> Result<impl IntoResponse, ApiError> {
    accepted(state.supervisor.restart().await)
}

fn accepted(
    result: Result<ActionAccepted, SupervisorError>,
) -> Result<impl IntoResponse, ApiError> {
    Ok((StatusCode::ACCEPTED, Json(result?)))
}

async fn events(
    State(state): State<AppState>,
) -> Sse<impl futures_util::Stream<Item = Result<Event, Infallible>>> {
    let receiver = state.supervisor.events.subscribe();
    let stream = stream::unfold(receiver, |mut receiver| async move {
        loop {
            match receiver.recv().await {
                Ok(event) => {
                    let item = Event::default()
                        .event(event.kind.clone())
                        .json_data(event)
                        .unwrap_or_else(|_| Event::default().event("serialization_error"));
                    return Some((Ok(item), receiver));
                }
                Err(tokio::sync::broadcast::error::RecvError::Lagged(skipped)) => {
                    let item = Event::default()
                        .event("lagged")
                        .data(json!({"skipped": skipped}).to_string());
                    return Some((Ok(item), receiver));
                }
                Err(tokio::sync::broadcast::error::RecvError::Closed) => return None,
            }
        }
    });
    Sse::new(stream).keep_alive(
        KeepAlive::new()
            .interval(Duration::from_secs(10))
            .text("keep-alive"),
    )
}

#[derive(Deserialize)]
struct LogsQuery {
    limit: Option<usize>,
}

async fn logs(State(state): State<AppState>, Query(query): Query<LogsQuery>) -> impl IntoResponse {
    let logs = state.supervisor.logs.lock().await;
    let limit = query.limit.unwrap_or(100).clamp(1, 200);
    Json(
        logs.iter()
            .skip(logs.len().saturating_sub(limit))
            .cloned()
            .collect::<Vec<_>>(),
    )
}

struct ApiError(SupervisorError);

struct WebRtcApiError(WebRtcError);

impl From<WebRtcError> for WebRtcApiError {
    fn from(value: WebRtcError) -> Self {
        Self(value)
    }
}

impl IntoResponse for WebRtcApiError {
    fn into_response(self) -> Response {
        let (status, code) = match self.0 {
            WebRtcError::Disabled => (StatusCode::SERVICE_UNAVAILABLE, "webrtc_disabled"),
            WebRtcError::NotReady => (StatusCode::SERVICE_UNAVAILABLE, "webrtc_not_ready"),
            WebRtcError::ClientLimit => (StatusCode::TOO_MANY_REQUESTS, "webrtc_client_limit"),
            WebRtcError::InvalidOffer(_) => (StatusCode::BAD_REQUEST, "webrtc_invalid_offer"),
            WebRtcError::Codec(_) => (StatusCode::BAD_REQUEST, "webrtc_codec"),
            WebRtcError::NotFound => (StatusCode::NOT_FOUND, "webrtc_session_not_found"),
            WebRtcError::Operation(_) => (StatusCode::INTERNAL_SERVER_ERROR, "webrtc_operation"),
        };
        (
            status,
            Json(json!({"error": {"code": code, "message": self.0.to_string()}})),
        )
            .into_response()
    }
}

impl From<SupervisorError> for ApiError {
    fn from(value: SupervisorError) -> Self {
        Self(value)
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        let status = match self.0 {
            SupervisorError::InvalidConfig(_) => StatusCode::BAD_REQUEST,
            SupervisorError::Conflict
            | SupervisorError::AlreadyRunning
            | SupervisorError::AlreadyStopped => StatusCode::CONFLICT,
            SupervisorError::Operation(_) => StatusCode::INTERNAL_SERVER_ERROR,
        };
        let code = match status {
            StatusCode::BAD_REQUEST => "invalid_config",
            StatusCode::CONFLICT => "state_conflict",
            _ => "operation_failed",
        };
        (
            status,
            Json(json!({"error": {"code": code, "message": self.0.to_string()}})),
        )
            .into_response()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{DaemonConfig, RtspConfig, WebRtcConfig};
    use crate::model::PersistentState;
    use crate::supervisor::spawn_supervisor;
    use axum::body::Body;
    use http_body_util::BodyExt;
    use tempfile::tempdir;
    use tower::ServiceExt;

    #[test]
    fn parses_http_byte_ranges() {
        assert_eq!(parse_range("bytes=10-19", 100), Some((10, 19)));
        assert_eq!(parse_range("bytes=90-", 100), Some((90, 99)));
        assert_eq!(parse_range("bytes=-10", 100), Some((90, 99)));
        assert_eq!(parse_range("bytes=100-", 100), None);
        assert_eq!(parse_range("bytes=0-1,3-4", 100), None);
    }

    #[tokio::test]
    async fn streams_zip64_without_a_temporary_archive() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("sample.mp4");
        tokio::fs::write(&path, b"sample-video").await.unwrap();
        let (mut reader, writer) = tokio::io::duplex(4096);
        let task = tokio::spawn(write_zip64(writer, vec![("sample.mp4".into(), path)]));
        let mut data = Vec::new();
        reader.read_to_end(&mut data).await.unwrap();
        task.await.unwrap().unwrap();
        assert!(data.starts_with(&0x04034b50_u32.to_le_bytes()));
        assert!(
            data.windows(4)
                .any(|item| item == 0x06064b50_u32.to_le_bytes())
        );
        assert!(data.windows(10).any(|item| item == b"sample.mp4"));
    }

    #[tokio::test]
    async fn exposes_status_and_rejects_invalid_config() {
        let temp = tempdir().unwrap();
        let settings = DaemonConfig {
            autostart: false,
            data_dir: temp.path().join("data"),
            runtime_dir: temp.path().join("run"),
            web_dir: temp.path().to_path_buf(),
            ..DaemonConfig::default()
        };
        let handle = spawn_supervisor(
            settings.clone(),
            PersistentState::new(WorkerConfig::default()),
        )
        .await;
        let recording = RecordingManager::new(
            settings.recording.clone(),
            &settings.data_dir,
            handle.preview.clone(),
            handle.events.clone(),
        )
        .await
        .unwrap();
        let rtsp = RtspServer::start(
            RtspConfig {
                enabled: false,
                ..RtspConfig::default()
            },
            handle.preview.clone(),
            handle.events.clone(),
        )
        .await
        .unwrap();
        let webrtc = WebRtcServer::start(
            WebRtcConfig {
                enabled: false,
                ..WebRtcConfig::default()
            },
            handle.preview.clone(),
            handle.events.clone(),
        )
        .await
        .unwrap();
        let app = router(handle.clone(), recording, rtsp, webrtc, temp.path());
        let response = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .uri("/api/v1/status")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);

        let mut invalid = WorkerConfig::default();
        invalid.video.width = 1;
        let response = app
            .oneshot(
                axum::http::Request::builder()
                    .method("PUT")
                    .uri("/api/v1/config")
                    .header("content-type", "application/json")
                    .body(Body::from(serde_json::to_vec(&invalid).unwrap()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::BAD_REQUEST);
        let body = response.into_body().collect().await.unwrap().to_bytes();
        assert!(String::from_utf8_lossy(&body).contains("invalid_config"));
        handle.shutdown().await;
    }
}
