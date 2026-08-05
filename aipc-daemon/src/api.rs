use crate::config::WorkerConfig;
use crate::preview::PreviewHub;
use crate::supervisor::{ActionAccepted, SupervisorError, SupervisorHandle};
use axum::Json;
use axum::Router;
use axum::extract::{Query, State, WebSocketUpgrade};
use axum::http::StatusCode;
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use futures_util::stream;
use serde::Deserialize;
use serde_json::json;
use std::convert::Infallible;
use std::path::Path;
use std::time::Duration;
use tower_http::services::{ServeDir, ServeFile};

#[derive(Clone)]
struct AppState {
    supervisor: SupervisorHandle,
    preview: PreviewHub,
}

pub fn router(supervisor: SupervisorHandle, web_dir: &Path) -> Router {
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
        .fallback_service(static_service)
        .with_state(AppState {
            preview: supervisor.preview.clone(),
            supervisor,
        })
}

async fn preview_status(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.preview.status())
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
    use crate::config::DaemonConfig;
    use crate::model::PersistentState;
    use crate::supervisor::spawn_supervisor;
    use axum::body::Body;
    use http_body_util::BodyExt;
    use tempfile::tempdir;
    use tower::ServiceExt;

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
        let handle =
            spawn_supervisor(settings, PersistentState::new(WorkerConfig::default())).await;
        let app = router(handle.clone(), temp.path());
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
