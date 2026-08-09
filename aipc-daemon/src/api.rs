use crate::ai_manager::{AiManager, AiProjectDocument, OsdMode};
use crate::config::{InputSourceConfig, UiConfig, WorkerConfig};
use crate::dependencies::DependencyManager;
use crate::preview::PreviewHub;
use crate::recording::{RecordingManager, RecordingSettingsUpdate};
use crate::rtsp::RtspServer;
use crate::source::SourceManager;
use crate::supervisor::{ActionAccepted, SupervisorError, SupervisorHandle};
use crate::webrtc::{WebRtcError, WebRtcServer};
use axum::Json;
use axum::Router;
use axum::body::Body;
use axum::extract::{
    ConnectInfo, DefaultBodyLimit, Multipart, Path as AxumPath, Query, State, WebSocketUpgrade,
};
use axum::http::{HeaderMap, StatusCode, header};
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::{IntoResponse, Response};
use axum::routing::{delete, get, post};
use futures_util::stream;
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::convert::Infallible;
use std::net::SocketAddr;
use std::path::Path;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncSeekExt, SeekFrom};
use tokio::sync::Mutex;
use tokio_util::io::ReaderStream;
use tower_http::services::{ServeDir, ServeFile};

mod ai;
mod dependencies;
mod zip64;
use ai::*;
use dependencies::*;
use zip64::write_zip64;

#[derive(Clone)]
struct AppState {
    supervisor: SupervisorHandle,
    preview: PreviewHub,
    recording: RecordingManager,
    rtsp: RtspServer,
    webrtc: WebRtcServer,
    ai: AiManager,
    dependencies: DependencyManager,
    sources: SourceManager,
    maintenance: Arc<Mutex<()>>,
    ui: UiConfig,
}

pub fn router(
    supervisor: SupervisorHandle,
    recording: RecordingManager,
    rtsp: RtspServer,
    webrtc: WebRtcServer,
    ai: AiManager,
    dependencies: DependencyManager,
    sources: SourceManager,
    ui: UiConfig,
    web_dir: &Path,
) -> Router {
    let index = web_dir.join("index.html");
    let static_service = ServeDir::new(web_dir).fallback(ServeFile::new(index));
    Router::new()
        .route("/healthz", get(healthz))
        .route("/api/v1/about", get(about))
        .route("/api/v1/status", get(status))
        .route("/api/v1/ai/status", get(ai_status))
        .route(
            "/api/v1/ai/projects",
            get(ai_projects).post(ai_project_create),
        )
        .route(
            "/api/v1/ai/projects/{id}",
            get(ai_project_get)
                .put(ai_project_put)
                .delete(ai_project_delete),
        )
        .route(
            "/api/v1/ai/projects/{id}/validate",
            post(ai_project_validate),
        )
        .route("/api/v1/ai/projects/{id}/deploy", post(ai_project_deploy))
        .route("/api/v1/ai/models", get(ai_models).post(ai_model_upload))
        .route("/api/v1/ai/models/{name}", delete(ai_model_delete))
        .route("/api/v1/ai/osd", get(ai_osd).put(ai_osd_update))
        .route("/api/v1/ai/events", get(ai_events))
        .route("/api/v1/ai/results/latest", get(ai_results_latest))
        .route("/api/v1/ai/results/stream", get(ai_results_stream))
        .route("/api/v1/ai/results/schema", get(ai_results_schema))
        .route("/api/v1/dependencies", get(dependency_list))
        .route(
            "/api/v1/dependencies/{id}/versions",
            post(dependency_version_upload),
        )
        .route(
            "/api/v1/dependencies/{id}/versions/{sha256}",
            delete(dependency_version_delete),
        )
        .route(
            "/api/v1/dependencies/{id}/activate",
            post(dependency_activate),
        )
        .route(
            "/api/v1/dependencies/{id}/rollback",
            post(dependency_rollback),
        )
        .route(
            "/api/v1/dependencies/{id}/factory",
            post(dependency_factory),
        )
        .route("/api/v1/config", get(config).put(apply_config))
        .route("/api/v1/worker/start", post(start))
        .route("/api/v1/worker/stop", post(stop))
        .route("/api/v1/worker/restart", post(restart))
        .route("/api/v1/events", get(events))
        .route("/api/v1/logs", get(logs))
        .route("/api/v1/preview/status", get(preview_status))
        .route("/api/v1/preview/ws", get(preview_ws))
        .route("/api/v1/sources", get(source_list))
        .route("/api/v1/sources/{id}", axum::routing::put(source_put))
        .route("/api/v1/sources/{id}/start", post(source_start))
        .route("/api/v1/sources/{id}/stop", post(source_stop))
        .route("/api/v1/sources/{id}/reconnect", post(source_reconnect))
        .route(
            "/api/v1/stream/active-source",
            axum::routing::put(source_set_active),
        )
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
        .layer(DefaultBodyLimit::max(130 * 1024 * 1024))
        .with_state(AppState {
            preview: supervisor.preview.clone(),
            recording,
            rtsp,
            webrtc,
            ai,
            dependencies,
            sources,
            maintenance: Arc::new(Mutex::new(())),
            ui,
            supervisor,
        })
}

async fn source_list(State(state): State<AppState>) -> impl IntoResponse {
    Json(json!({
        "enabled": state.sources.enabled(),
        "active_source": state.sources.active_id(),
        "sources": state.sources.list(),
    }))
}

async fn source_put(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    Json(source): Json<InputSourceConfig>,
) -> Result<impl IntoResponse, SourceApiError> {
    if source.id != id {
        return Err(SourceApiError::bad_request(
            "source id in path and body must match",
        ));
    }
    state
        .sources
        .upsert(source)
        .await
        .map_err(SourceApiError::from)?;
    Ok(Json(json!({"source_id": id, "updated": true})))
}

async fn source_start(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<impl IntoResponse, SourceApiError> {
    state
        .sources
        .start(&id)
        .await
        .map_err(SourceApiError::from)?;
    Ok((StatusCode::ACCEPTED, Json(json!({"source_id": id}))))
}

async fn source_stop(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<impl IntoResponse, SourceApiError> {
    state
        .sources
        .stop(&id)
        .await
        .map_err(SourceApiError::from)?;
    Ok((StatusCode::ACCEPTED, Json(json!({"source_id": id}))))
}

async fn source_reconnect(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<impl IntoResponse, SourceApiError> {
    state
        .sources
        .reconnect(&id)
        .await
        .map_err(SourceApiError::from)?;
    Ok((StatusCode::ACCEPTED, Json(json!({"source_id": id}))))
}

#[derive(Deserialize)]
struct ActiveSourceUpdate {
    source_id: String,
}

async fn source_set_active(
    State(state): State<AppState>,
    Json(update): Json<ActiveSourceUpdate>,
) -> Result<impl IntoResponse, SourceApiError> {
    state
        .sources
        .set_active(&update.source_id)
        .await
        .map_err(SourceApiError::from)?;
    Ok(Json(json!({"active_source": update.source_id})))
}

#[derive(Serialize)]
struct AboutResponse {
    #[serde(flatten)]
    ui: UiConfig,
    daemon_version: &'static str,
}

async fn about(State(state): State<AppState>) -> impl IntoResponse {
    Json(AboutResponse {
        ui: state.ui,
        daemon_version: env!("CARGO_PKG_VERSION"),
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

    fn forbidden(message: impl Into<String>) -> Self {
        Self {
            status: StatusCode::FORBIDDEN,
            message: message.into(),
        }
    }

    fn conflict(message: impl Into<String>) -> Self {
        Self {
            status: StatusCode::CONFLICT,
            message: message.into(),
        }
    }
}

impl<E: Into<anyhow::Error>> From<E> for AppError {
    fn from(error: E) -> Self {
        let message = error.into().to_string();
        let status = if message.contains("not found") {
            StatusCode::NOT_FOUND
        } else if message.contains("dependency management is disabled") {
            StatusCode::FORBIDDEN
        } else if message.contains("already")
            || message.contains("while recording")
            || message.contains("not active")
            || message.contains("operation")
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
    let _maintenance = state
        .maintenance
        .try_lock()
        .map_err(|_| ApiError(SupervisorError::Conflict))?;
    let accepted = state.supervisor.apply(config).await?;
    Ok((StatusCode::ACCEPTED, Json(accepted)))
}

async fn start(State(state): State<AppState>) -> Result<impl IntoResponse, ApiError> {
    let _maintenance = state
        .maintenance
        .try_lock()
        .map_err(|_| ApiError(SupervisorError::Conflict))?;
    accepted(state.supervisor.start().await)
}

async fn stop(State(state): State<AppState>) -> Result<impl IntoResponse, ApiError> {
    let _maintenance = state
        .maintenance
        .try_lock()
        .map_err(|_| ApiError(SupervisorError::Conflict))?;
    accepted(state.supervisor.stop().await)
}

async fn restart(State(state): State<AppState>) -> Result<impl IntoResponse, ApiError> {
    let _maintenance = state
        .maintenance
        .try_lock()
        .map_err(|_| ApiError(SupervisorError::Conflict))?;
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

struct SourceApiError {
    status: StatusCode,
    message: String,
}

impl SourceApiError {
    fn bad_request(message: impl Into<String>) -> Self {
        Self {
            status: StatusCode::BAD_REQUEST,
            message: message.into(),
        }
    }
}

impl From<anyhow::Error> for SourceApiError {
    fn from(error: anyhow::Error) -> Self {
        let message = error.to_string();
        let status = if message.contains("not found") {
            StatusCode::NOT_FOUND
        } else if message.contains("outside")
            || message.contains("invalid")
            || message.contains("must")
        {
            StatusCode::BAD_REQUEST
        } else {
            StatusCode::INTERNAL_SERVER_ERROR
        };
        Self { status, message }
    }
}

impl IntoResponse for SourceApiError {
    fn into_response(self) -> Response {
        (
            self.status,
            Json(json!({"error": {"code": "source_error", "message": self.message}})),
        )
            .into_response()
    }
}

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
    use crate::ai_results::{
        AiBoundingBoxV1, AiFrameInfoV1, AiInferenceInfoV1, AiObjectV1, AiResultInput,
    };
    use crate::config::{AiDaemonConfig, DaemonConfig, RtspConfig, WebRtcConfig};
    use crate::model::PersistentState;
    use crate::supervisor::spawn_supervisor;
    use axum::body::Body;
    use http_body_util::BodyExt;
    use tempfile::tempdir;
    use tower::ServiceExt;

    fn ai_result_input(sequence: u64) -> AiResultInput {
        AiResultInput {
            source_id: "camera0".into(),
            media_generation: "media-test".into(),
            ai_generation: "ai-test".into(),
            sequence,
            pts_us: sequence * 100_000,
            published_at_ms: 1000 + sequence * 100,
            frame: AiFrameInfoV1 {
                width: 1920,
                height: 1080,
                coordinate_space: "main_normalized_top_left".into(),
            },
            inference: AiInferenceInfoV1 {
                project: "test".into(),
                algorithm: "yolov5".into(),
                model: "test.rknn".into(),
                duration_us: 100,
            },
            objects: vec![AiObjectV1 {
                track_id: 1,
                class_id: 0,
                label: "person".into(),
                confidence: 0.9,
                bbox: AiBoundingBoxV1 {
                    x: 0.1,
                    y: 0.2,
                    width: 0.3,
                    height: 0.4,
                },
            }],
            annotations: vec![],
        }
    }

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
            None,
        )
        .await
        .unwrap();
        let ai = AiManager::new(
            AiDaemonConfig {
                enabled: false,
                ..AiDaemonConfig::default()
            },
            &settings.data_dir,
            handle.ai.clone(),
            handle.events.clone(),
        )
        .await
        .unwrap();
        let ui = UiConfig {
            platform_name: "RK3576".into(),
            board_name: "Development board".into(),
            ..UiConfig::default()
        };
        let mut dependency_config = settings.dependencies.clone();
        dependency_config.root = settings.data_dir.join("dependencies");
        let dependencies = DependencyManager::new(
            dependency_config,
            temp.path(),
            settings.worker_path.clone(),
            settings.ai.worker_path.clone(),
            handle.clone(),
            ai.clone(),
            handle.events.clone(),
        )
        .await
        .unwrap();
        let mut input_config = settings.input.clone();
        input_config.file_roots = vec![temp.path().to_path_buf()];
        let sources = SourceManager::new(
            input_config,
            handle.preview.clone(),
            handle.ai.clone(),
            handle.events.clone(),
        )
        .unwrap();
        let app = router(
            handle.clone(),
            recording,
            rtsp,
            webrtc,
            ai.clone(),
            dependencies,
            sources,
            ui,
            temp.path(),
        );
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

        let response = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .uri("/api/v1/sources")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let body = response.into_body().collect().await.unwrap().to_bytes();
        let sources: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(sources["enabled"], false);
        assert_eq!(sources["sources"], json!([]));

        let response = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .uri("/api/v1/about")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let body = response.into_body().collect().await.unwrap().to_bytes();
        let about: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(about["project_name"], "AIPC");
        assert_eq!(about["platform_name"], "RK3576");
        assert_eq!(about["board_name"], "Development board");
        assert_eq!(about["daemon_version"], env!("CARGO_PKG_VERSION"));

        let mut invalid = WorkerConfig::default();
        invalid.video.width = 1;
        let response = app
            .clone()
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

        let dependency_list = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .uri("/api/v1/dependencies")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(dependency_list.status(), StatusCode::OK);
        let body = dependency_list
            .into_body()
            .collect()
            .await
            .unwrap()
            .to_bytes();
        let dependencies: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(dependencies["enabled"], false);
        assert_eq!(dependencies["items"].as_array().unwrap().len(), 9);

        let disabled_activation = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .method("POST")
                    .uri("/api/v1/dependencies/rknn-runtime/factory")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(disabled_activation.status(), StatusCode::FORBIDDEN);

        let latest = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .uri("/api/v1/ai/results/latest")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(latest.status(), StatusCode::NO_CONTENT);

        ai.publish_test_result(ai_result_input(1));
        let latest = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .uri("/api/v1/ai/results/latest")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(latest.status(), StatusCode::OK);
        let latest_body = latest.into_body().collect().await.unwrap().to_bytes();
        let latest_json: serde_json::Value = serde_json::from_slice(&latest_body).unwrap();
        assert_eq!(latest_json["type"], "io.aipc.ai.frame.v1");
        assert_eq!(latest_json["data"]["objects"][0]["bbox"]["x"], 0.1);
        let latest_id = latest_json["id"].as_str().unwrap().to_owned();

        let schema = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .uri("/api/v1/ai/results/schema")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(schema.status(), StatusCode::OK);
        assert_eq!(
            schema.headers().get(header::CONTENT_TYPE).unwrap(),
            "application/schema+json"
        );
        let schema_body = schema.into_body().collect().await.unwrap().to_bytes();
        let schema_json: serde_json::Value = serde_json::from_slice(&schema_body).unwrap();
        assert_eq!(schema_json["$id"], "/api/v1/ai/results/schema");

        let stream_response = app
            .clone()
            .oneshot(
                axum::http::Request::builder()
                    .uri("/api/v1/ai/results/stream")
                    .header("last-event-id", latest_id)
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(stream_response.status(), StatusCode::OK);
        assert!(
            stream_response.headers()[header::CONTENT_TYPE]
                .to_str()
                .unwrap()
                .starts_with("text/event-stream")
        );
        ai.publish_test_result(ai_result_input(2));
        let mut stream_body = stream_response.into_body();
        let frame = tokio::time::timeout(Duration::from_secs(1), stream_body.frame())
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        let data = frame.into_data().unwrap();
        let text = String::from_utf8_lossy(&data);
        assert!(text.contains("event: io.aipc.ai.frame.v1"));
        assert!(text.contains("\"sequence\":2"));

        handle.shutdown().await;
    }
}
