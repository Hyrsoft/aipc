use super::*;

pub(super) async fn ai_status(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.ai.status())
}

pub(super) async fn ai_projects(
    State(state): State<AppState>,
) -> Result<impl IntoResponse, AppError> {
    Ok(Json(state.ai.list_projects().await?))
}

pub(super) async fn ai_project_create(
    State(state): State<AppState>,
    Json(document): Json<AiProjectDocument>,
) -> Result<impl IntoResponse, AppError> {
    let id = document.manifest.id.clone();
    Ok((
        StatusCode::CREATED,
        Json(state.ai.put_project(&id, document).await?),
    ))
}

pub(super) async fn ai_project_get(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<impl IntoResponse, AppError> {
    Ok(Json(state.ai.get_project(&id).await?))
}

pub(super) async fn ai_project_put(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    Json(document): Json<AiProjectDocument>,
) -> Result<impl IntoResponse, AppError> {
    Ok(Json(state.ai.put_project(&id, document).await?))
}

pub(super) async fn ai_project_delete(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<StatusCode, AppError> {
    state.ai.delete_project(&id).await?;
    Ok(StatusCode::NO_CONTENT)
}

pub(super) async fn ai_project_validate(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<impl IntoResponse, AppError> {
    Ok(Json(state.ai.validate_project(&id).await?))
}

pub(super) async fn ai_project_deploy(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<impl IntoResponse, AppError> {
    Ok((StatusCode::ACCEPTED, Json(state.ai.deploy(&id).await?)))
}

pub(super) async fn ai_models(
    State(state): State<AppState>,
) -> Result<impl IntoResponse, AppError> {
    Ok(Json(state.ai.list_models().await?))
}

pub(super) async fn ai_model_upload(
    State(state): State<AppState>,
    mut multipart: Multipart,
) -> Result<impl IntoResponse, AppError> {
    let mut name = None;
    let mut data = None;
    while let Some(field) = multipart.next_field().await? {
        if field.name() == Some("file") {
            name = field.file_name().map(ToOwned::to_owned);
            data = Some(field.bytes().await?);
        }
    }
    let name = name.ok_or_else(|| AppError::bad_request("multipart file name is required"))?;
    let data = data.ok_or_else(|| AppError::bad_request("multipart file field is required"))?;
    Ok((
        StatusCode::CREATED,
        Json(state.ai.put_model(&name, &data).await?),
    ))
}

pub(super) async fn ai_model_delete(
    State(state): State<AppState>,
    AxumPath(name): AxumPath<String>,
) -> Result<StatusCode, AppError> {
    state.ai.delete_model(&name).await?;
    Ok(StatusCode::NO_CONTENT)
}

pub(super) async fn ai_osd(State(state): State<AppState>) -> impl IntoResponse {
    Json(json!({"mode": state.ai.status().osd_mode}))
}

#[derive(Deserialize)]
pub(super) struct OsdUpdate {
    mode: OsdMode,
}

pub(super) async fn ai_osd_update(
    State(state): State<AppState>,
    Json(update): Json<OsdUpdate>,
) -> Result<impl IntoResponse, AppError> {
    Ok(Json(
        json!({"mode": state.ai.set_osd_mode(update.mode).await?}),
    ))
}

pub(super) async fn ai_events(
    State(state): State<AppState>,
) -> Sse<impl futures_util::Stream<Item = Result<Event, Infallible>>> {
    let receiver = state.ai.subscribe_metadata();
    let stream = stream::unfold(receiver, |mut receiver| async move {
        loop {
            match receiver.recv().await {
                Ok(metadata) => {
                    let item = Event::default()
                        .event("detections")
                        .json_data(&*metadata)
                        .unwrap_or_else(|_| Event::default().event("serialization_error"));
                    return Some((Ok(item), receiver));
                }
                Err(tokio::sync::broadcast::error::RecvError::Lagged(skipped)) => {
                    return Some((
                        Ok(Event::default()
                            .event("lagged")
                            .data(json!({"skipped": skipped}).to_string())),
                        receiver,
                    ));
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
