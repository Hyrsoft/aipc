use super::*;

#[derive(Deserialize)]
pub(super) struct ActivateDependency {
    sha256: String,
}

pub(super) async fn dependency_list(
    State(state): State<AppState>,
) -> Result<impl IntoResponse, AppError> {
    Ok(Json(state.dependencies.list().await?))
}

pub(super) async fn dependency_version_upload(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    mut multipart: Multipart,
) -> Result<impl IntoResponse, AppError> {
    if !state.dependencies.enabled() {
        return Err(AppError::forbidden("dependency management is disabled"));
    }
    let mut data = None;
    while let Some(field) = multipart.next_field().await? {
        if field.name() == Some("file") {
            data = Some(field.bytes().await?);
        }
    }
    let data = data.ok_or_else(|| AppError::bad_request("multipart file field is required"))?;
    Ok((
        StatusCode::CREATED,
        Json(state.dependencies.upload(&id, &data).await?),
    ))
}

pub(super) async fn dependency_version_delete(
    State(state): State<AppState>,
    AxumPath((id, sha256)): AxumPath<(String, String)>,
) -> Result<StatusCode, AppError> {
    if !state.dependencies.enabled() {
        return Err(AppError::forbidden("dependency management is disabled"));
    }
    state.dependencies.delete(&id, &sha256).await?;
    Ok(StatusCode::NO_CONTENT)
}

pub(super) async fn dependency_activate(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
    Json(request): Json<ActivateDependency>,
) -> Result<impl IntoResponse, AppError> {
    let _maintenance = state
        .maintenance
        .try_lock()
        .map_err(|_| AppError::conflict("another worker maintenance operation is running"))?;
    Ok(Json(
        state.dependencies.activate(&id, &request.sha256).await?,
    ))
}

pub(super) async fn dependency_rollback(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<impl IntoResponse, AppError> {
    let _maintenance = state
        .maintenance
        .try_lock()
        .map_err(|_| AppError::conflict("another worker maintenance operation is running"))?;
    Ok(Json(state.dependencies.rollback(&id).await?))
}

pub(super) async fn dependency_factory(
    State(state): State<AppState>,
    AxumPath(id): AxumPath<String>,
) -> Result<impl IntoResponse, AppError> {
    let _maintenance = state
        .maintenance
        .try_lock()
        .map_err(|_| AppError::conflict("another worker maintenance operation is running"))?;
    Ok(Json(state.dependencies.restore_factory(&id).await?))
}
