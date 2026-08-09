use crate::ai_manager::{AiManager, AiProcessState};
use crate::config::DependencyConfig;
use crate::model::{ProcessState, ServerEvent, now_ms};
use crate::supervisor::{SupervisorError, SupervisorHandle};
use anyhow::{Context, bail, ensure};
use serde::{Deserialize, Serialize};
use serde_json::json;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};
use std::time::Duration;
use tokio::process::Command;
use tokio::sync::{Mutex, broadcast};
use uuid::Uuid;

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum DependencyOwner {
    AiWorker,
    MediaWorker,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DependencyVersion {
    pub sha256: String,
    pub bytes: u64,
    pub soname: String,
    pub build_id: Option<String>,
    pub detected_version: Option<String>,
    pub needed: Vec<String>,
    pub source: String,
    pub uploaded_at_ms: Option<u64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct DependencyInfo {
    pub id: String,
    pub display_name: String,
    pub load_names: Vec<String>,
    pub owners: Vec<DependencyOwner>,
    pub state: String,
    pub factory: Option<DependencyVersion>,
    pub active: Option<DependencyVersion>,
    pub previous: Option<DependencyVersion>,
    pub versions: Vec<DependencyVersion>,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct DependencyList {
    pub enabled: bool,
    pub max_upload_bytes: u64,
    pub items: Vec<DependencyInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
struct SelectionRef {
    sha256: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
struct LibrarySelection {
    active: Option<String>,
    previous: Option<SelectionRef>,
    last_error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
struct PersistedDependencies {
    schema_version: u32,
    libraries: BTreeMap<String, LibrarySelection>,
}

impl Default for PersistedDependencies {
    fn default() -> Self {
        Self {
            schema_version: 1,
            libraries: BTreeMap::new(),
        }
    }
}

struct LibrarySpec {
    id: &'static str,
    display_name: &'static str,
    soname: &'static str,
    load_names: &'static [&'static str],
    owners: &'static [DependencyOwner],
    factory_names: &'static [&'static str],
    app_factory: bool,
}

const AI: &[DependencyOwner] = &[DependencyOwner::AiWorker];
const MEDIA_AI: &[DependencyOwner] = &[DependencyOwner::MediaWorker, DependencyOwner::AiWorker];

const LIBRARIES: &[LibrarySpec] = &[
    LibrarySpec {
        id: "rknn-runtime",
        display_name: "RKNN Runtime",
        soname: "librknnmrt.so",
        load_names: &["librknnmrt.so"],
        owners: AI,
        factory_names: &["librknnmrt.so"],
        app_factory: true,
    },
    LibrarySpec {
        id: "visiong",
        display_name: "VisionG",
        soname: "libvisiong.so",
        load_names: &["libvisiong.so"],
        owners: AI,
        factory_names: &["libvisiong.so"],
        app_factory: true,
    },
    LibrarySpec {
        id: "rockiva",
        display_name: "RockIVA",
        soname: "librockiva.so",
        load_names: &["librockiva.so"],
        owners: AI,
        factory_names: &["librockiva.so"],
        app_factory: false,
    },
    LibrarySpec {
        id: "rve",
        display_name: "Rockchip RVE",
        soname: "librve.so",
        load_names: &["librve.so"],
        owners: AI,
        factory_names: &["librve.so"],
        app_factory: false,
    },
    LibrarySpec {
        id: "ivs",
        display_name: "Rockchip IVS",
        soname: "libivs.so",
        load_names: &["libivs.so"],
        owners: AI,
        factory_names: &["libivs.so"],
        app_factory: false,
    },
    LibrarySpec {
        id: "rga",
        display_name: "Rockchip RGA",
        soname: "librga.so",
        load_names: &["librga.so"],
        owners: MEDIA_AI,
        factory_names: &["librga.so"],
        app_factory: false,
    },
    LibrarySpec {
        id: "mpp",
        display_name: "Rockchip MPP",
        soname: "librockchip_mpp.so.1",
        load_names: &["librockchip_mpp.so.1", "librockchip_mpp.so"],
        owners: MEDIA_AI,
        factory_names: &["librockchip_mpp.so.1", "librockchip_mpp.so.0"],
        app_factory: false,
    },
    LibrarySpec {
        id: "rkaiq",
        display_name: "Rockchip RKAIQ",
        soname: "librkaiq.so",
        load_names: &["librkaiq.so"],
        owners: MEDIA_AI,
        factory_names: &["librkaiq.so"],
        app_factory: false,
    },
    LibrarySpec {
        id: "rockit",
        display_name: "Rockit",
        soname: "librockit.so",
        load_names: &["librockit.so"],
        owners: MEDIA_AI,
        factory_names: &["librockit.so"],
        app_factory: false,
    },
];

struct Inner {
    config: DependencyConfig,
    versions: PathBuf,
    active: PathBuf,
    state_path: PathBuf,
    app_lib: PathBuf,
    media_worker: PathBuf,
    ai_worker: PathBuf,
    supervisor: SupervisorHandle,
    ai: AiManager,
    events: broadcast::Sender<ServerEvent>,
    persisted: Mutex<PersistedDependencies>,
    operations: Mutex<()>,
    operation_state: RwLock<BTreeMap<String, String>>,
}

#[derive(Clone)]
pub struct DependencyManager {
    inner: Arc<Inner>,
}

impl DependencyManager {
    pub async fn recover(config: &DependencyConfig) -> anyhow::Result<()> {
        let versions = config.root.join("versions");
        let active = config.root.join("active");
        tokio::fs::create_dir_all(&versions).await?;
        tokio::fs::create_dir_all(&active).await?;
        cleanup_parts(&versions).await?;
        let state = load_state(&config.root.join("state.json")).await?;
        for spec in LIBRARIES {
            let selection = state.libraries.get(spec.id).cloned().unwrap_or_default();
            apply_selection_paths(&active, &versions, spec, selection.active.as_deref()).await?;
        }
        Ok(())
    }

    pub async fn new(
        config: DependencyConfig,
        executable_dir: &Path,
        media_worker: PathBuf,
        ai_worker: PathBuf,
        supervisor: SupervisorHandle,
        ai: AiManager,
        events: broadcast::Sender<ServerEvent>,
    ) -> anyhow::Result<Self> {
        Self::recover(&config).await?;
        let persisted = load_state(&config.root.join("state.json")).await?;
        let app_lib = executable_dir
            .parent()
            .unwrap_or(executable_dir)
            .join("lib");
        Ok(Self {
            inner: Arc::new(Inner {
                versions: config.root.join("versions"),
                active: config.root.join("active"),
                state_path: config.root.join("state.json"),
                app_lib,
                media_worker,
                ai_worker,
                supervisor,
                ai,
                events,
                persisted: Mutex::new(persisted),
                operations: Mutex::new(()),
                operation_state: RwLock::new(BTreeMap::new()),
                config,
            }),
        })
    }

    pub fn enabled(&self) -> bool {
        self.inner.config.enabled
    }

    pub async fn list(&self) -> anyhow::Result<DependencyList> {
        let mut items = Vec::with_capacity(LIBRARIES.len());
        for spec in LIBRARIES {
            items.push(self.info_for(spec).await?);
        }
        Ok(DependencyList {
            enabled: self.enabled(),
            max_upload_bytes: self.inner.config.max_upload_bytes,
            items,
        })
    }

    pub async fn upload(&self, id: &str, data: &[u8]) -> anyhow::Result<DependencyVersion> {
        self.ensure_enabled()?;
        let spec = spec(id)?;
        ensure!(!data.is_empty(), "dependency upload is empty");
        ensure!(
            data.len() as u64 <= self.inner.config.max_upload_bytes,
            "dependency exceeds configured size limit"
        );
        let inspected = inspect_elf(data, spec.soname)?;
        let sha256 = hex(&Sha256::digest(data));
        let version = DependencyVersion {
            sha256: sha256.clone(),
            bytes: data.len() as u64,
            soname: inspected.soname,
            build_id: inspected.build_id,
            detected_version: if spec.id == "rknn-runtime" {
                detect_rknn_version(data)
            } else {
                None
            },
            needed: inspected.needed,
            source: "uploaded".into(),
            uploaded_at_ms: Some(now_ms()),
        };
        let target = self.inner.versions.join(spec.id).join(&sha256);
        if tokio::fs::metadata(&target).await.is_ok() {
            return read_version(&target.join("version.json")).await;
        }
        let parent = self.inner.versions.join(spec.id);
        tokio::fs::create_dir_all(&parent).await?;
        let staging = parent.join(format!(".{}.{}.part", sha256, Uuid::new_v4()));
        tokio::fs::create_dir_all(&staging).await?;
        write_synced(&staging.join(spec.soname), data).await?;
        write_json_synced(&staging.join("version.json"), &version).await?;
        tokio::fs::rename(&staging, &target).await?;
        sync_dir(&parent).await?;
        let _ = self.inner.events.send(ServerEvent::new(
            "dependency_uploaded",
            json!({"library": spec.id, "sha256": sha256}),
        ));
        Ok(version)
    }

    pub async fn delete(&self, id: &str, sha256: &str) -> anyhow::Result<()> {
        self.ensure_enabled()?;
        validate_sha(sha256)?;
        let spec = spec(id)?;
        let state = self.inner.persisted.lock().await;
        let selection = state.libraries.get(spec.id).cloned().unwrap_or_default();
        ensure!(
            selection.active.as_deref() != Some(sha256),
            "active dependency version cannot be deleted"
        );
        ensure!(
            selection
                .previous
                .as_ref()
                .and_then(|value| value.sha256.as_deref())
                != Some(sha256),
            "previous dependency version cannot be deleted"
        );
        drop(state);
        let target = self.inner.versions.join(spec.id).join(sha256);
        ensure!(
            tokio::fs::metadata(&target).await.is_ok(),
            "dependency version not found"
        );
        tokio::fs::remove_dir_all(target).await?;
        Ok(())
    }

    pub async fn activate(&self, id: &str, sha256: &str) -> anyhow::Result<DependencyInfo> {
        validate_sha(sha256)?;
        self.switch(
            id,
            SelectionRef {
                sha256: Some(sha256.into()),
            },
        )
        .await
    }

    pub async fn rollback(&self, id: &str) -> anyhow::Result<DependencyInfo> {
        self.ensure_enabled()?;
        let spec = spec(id)?;
        let state = self.inner.persisted.lock().await;
        let target = state
            .libraries
            .get(spec.id)
            .and_then(|value| value.previous.clone())
            .context("no previous dependency version")?;
        drop(state);
        self.switch(id, target).await
    }

    pub async fn restore_factory(&self, id: &str) -> anyhow::Result<DependencyInfo> {
        self.switch(id, SelectionRef { sha256: None }).await
    }

    async fn switch(&self, id: &str, target: SelectionRef) -> anyhow::Result<DependencyInfo> {
        self.ensure_enabled()?;
        let spec = spec(id)?;
        let _operation = self
            .inner
            .operations
            .try_lock()
            .context("dependency operation already in progress")?;
        ensure!(
            !self
                .inner
                .supervisor
                .status
                .borrow()
                .state
                .is_transitioning()
                && !matches!(
                    self.inner.ai.status().state,
                    AiProcessState::Starting | AiProcessState::RollingBack
                ),
            "another worker maintenance operation is already in progress"
        );
        if let Some(sha256) = target.sha256.as_deref() {
            ensure!(
                tokio::fs::metadata(
                    self.inner
                        .versions
                        .join(spec.id)
                        .join(sha256)
                        .join(spec.soname)
                )
                .await
                .is_ok(),
                "dependency version not found"
            );
        }
        let old_state = {
            let state = self.inner.persisted.lock().await;
            state.libraries.get(spec.id).cloned().unwrap_or_default()
        };
        if old_state.active == target.sha256 {
            return self.info_for(spec).await;
        }
        self.set_operation(spec.id, "validating");
        if let Some(sha256) = target.sha256.as_deref() {
            self.preflight(spec, &self.inner.versions.join(spec.id).join(sha256))
                .await?;
        }
        self.set_operation(spec.id, "restarting");
        apply_selection_paths(
            &self.inner.active,
            &self.inner.versions,
            spec,
            target.sha256.as_deref(),
        )
        .await?;
        match self.restart_owners(spec).await {
            Ok(()) => {
                let mut state = self.inner.persisted.lock().await;
                let active = {
                    let selection = state.libraries.entry(spec.id.into()).or_default();
                    selection.previous = Some(SelectionRef {
                        sha256: old_state.active,
                    });
                    selection.active = target.sha256;
                    selection.last_error = None;
                    selection.active.clone()
                };
                persist_state(&self.inner.state_path, &state).await?;
                self.set_operation(spec.id, "idle");
                let _ = self.inner.events.send(ServerEvent::new(
                    "dependency_activated",
                    json!({"library": spec.id, "active": active}),
                ));
                drop(state);
                self.info_for(spec).await
            }
            Err(candidate_error) => {
                self.set_operation(spec.id, "rolling_back");
                apply_selection_paths(
                    &self.inner.active,
                    &self.inner.versions,
                    spec,
                    old_state.active.as_deref(),
                )
                .await?;
                let rollback = self.restart_owners(spec).await;
                let message = match rollback {
                    Ok(()) => format!("candidate failed and was rolled back: {candidate_error}"),
                    Err(error) => {
                        format!("candidate failed: {candidate_error}; rollback failed: {error}")
                    }
                };
                let mut state = self.inner.persisted.lock().await;
                let selection = state.libraries.entry(spec.id.into()).or_default();
                *selection = old_state;
                selection.last_error = Some(message.clone());
                persist_state(&self.inner.state_path, &state).await?;
                self.set_operation(
                    spec.id,
                    if message.contains("rollback failed") {
                        "degraded"
                    } else {
                        "idle"
                    },
                );
                let _ = self.inner.events.send(ServerEvent::new(
                    "dependency_rollback",
                    json!({"library": spec.id, "error": message}),
                ));
                bail!(message)
            }
        }
    }

    async fn preflight(&self, spec: &LibrarySpec, candidate_dir: &Path) -> anyhow::Result<()> {
        let mut paths = vec![candidate_dir.to_path_buf(), self.inner.active.clone()];
        if let Some(current) = std::env::var_os("LD_LIBRARY_PATH") {
            paths.extend(std::env::split_paths(&current));
        }
        let library_path = std::env::join_paths(paths)?.to_string_lossy().into_owned();
        for (owner, worker) in [
            (DependencyOwner::MediaWorker, &self.inner.media_worker),
            (DependencyOwner::AiWorker, &self.inner.ai_worker),
        ] {
            if !spec.owners.contains(&owner) {
                continue;
            }
            let output = tokio::time::timeout(
                Duration::from_secs(10),
                Command::new(worker)
                    .arg("--probe-load")
                    .env("LD_LIBRARY_PATH", &library_path)
                    .output(),
            )
            .await
            .context("dependency worker preflight timed out")??;
            ensure!(
                output.status.success(),
                "{} preflight failed: {}",
                worker.display(),
                String::from_utf8_lossy(&output.stderr).trim()
            );
        }
        Ok(())
    }

    async fn restart_owners(&self, spec: &LibrarySpec) -> anyhow::Result<()> {
        let owns_media = spec.owners.contains(&DependencyOwner::MediaWorker);
        let owns_ai = spec.owners.contains(&DependencyOwner::AiWorker);
        let project = if owns_media {
            self.inner.ai.stop_for_maintenance().await
        } else {
            None
        };
        if owns_media {
            self.restart_media().await?;
        }
        if owns_ai {
            if owns_media {
                self.inner.ai.start_for_maintenance(project).await?;
            } else {
                self.inner.ai.restart_for_maintenance().await?;
            }
        }
        Ok(())
    }

    async fn restart_media(&self) -> anyhow::Result<()> {
        let accepted = loop {
            match self.inner.supervisor.restart().await {
                Ok(value) => break value,
                Err(SupervisorError::Conflict) => {
                    tokio::time::sleep(Duration::from_millis(250)).await
                }
                Err(error) => return Err(error.into()),
            }
        };
        let generation = accepted
            .generation
            .context("media restart returned no generation")?;
        let mut status = self.inner.supervisor.status.clone();
        tokio::time::timeout(Duration::from_secs(35), async {
            loop {
                let current = status.borrow().clone();
                if current.generation.as_deref() == Some(&generation) {
                    if current.state == ProcessState::Failed {
                        bail!(
                            current
                                .last_error
                                .unwrap_or_else(|| "media worker failed".into())
                        );
                    }
                    if current.state == ProcessState::Running
                        && current.video_ready
                        && self.inner.supervisor.preview.status().available
                    {
                        return Ok(());
                    }
                }
                status.changed().await.context("media supervisor stopped")?;
            }
        })
        .await
        .context("media worker dependency restart timed out")??;
        Ok(())
    }

    async fn info_for(&self, spec: &LibrarySpec) -> anyhow::Result<DependencyInfo> {
        let selection = {
            let state = self.inner.persisted.lock().await;
            state.libraries.get(spec.id).cloned().unwrap_or_default()
        };
        let factory = self.factory_version(spec).await;
        let versions = self.list_versions(spec).await?;
        let find = |sha: &str| versions.iter().find(|value| value.sha256 == sha).cloned();
        let active = selection
            .active
            .as_deref()
            .and_then(find)
            .or_else(|| factory.clone());
        let previous = match selection.previous {
            Some(SelectionRef { sha256: Some(sha) }) => find(&sha),
            Some(SelectionRef { sha256: None }) => factory.clone(),
            None => None,
        };
        Ok(DependencyInfo {
            id: spec.id.into(),
            display_name: spec.display_name.into(),
            load_names: spec
                .load_names
                .iter()
                .map(|value| (*value).into())
                .collect(),
            owners: spec.owners.to_vec(),
            state: self
                .inner
                .operation_state
                .read()
                .unwrap()
                .get(spec.id)
                .cloned()
                .unwrap_or_else(|| "idle".into()),
            factory,
            active,
            previous,
            versions,
            last_error: selection.last_error,
        })
    }

    async fn list_versions(&self, spec: &LibrarySpec) -> anyhow::Result<Vec<DependencyVersion>> {
        let directory = self.inner.versions.join(spec.id);
        let mut versions = Vec::new();
        let mut reader = match tokio::fs::read_dir(&directory).await {
            Ok(value) => value,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(versions),
            Err(error) => return Err(error.into()),
        };
        while let Some(entry) = reader.next_entry().await? {
            if !entry.file_type().await?.is_dir()
                || entry.file_name().to_string_lossy().starts_with('.')
            {
                continue;
            }
            if let Ok(version) = read_version(&entry.path().join("version.json")).await {
                versions.push(version);
            }
        }
        versions.sort_by(|left, right| right.uploaded_at_ms.cmp(&left.uploaded_at_ms));
        Ok(versions)
    }

    async fn factory_version(&self, spec: &LibrarySpec) -> Option<DependencyVersion> {
        for name in spec.factory_names {
            let path = if spec.app_factory {
                self.inner.app_lib.join(name)
            } else {
                Path::new("/oem/usr/lib").join(name)
            };
            let Ok(data) = tokio::fs::read(&path).await else {
                continue;
            };
            let Ok(inspected) = inspect_elf(&data, spec.soname) else {
                continue;
            };
            return Some(DependencyVersion {
                sha256: hex(&Sha256::digest(&data)),
                bytes: data.len() as u64,
                soname: inspected.soname,
                build_id: inspected.build_id,
                detected_version: if spec.id == "rknn-runtime" {
                    detect_rknn_version(&data)
                } else {
                    None
                },
                needed: inspected.needed,
                source: "factory".into(),
                uploaded_at_ms: None,
            });
        }
        None
    }

    fn ensure_enabled(&self) -> anyhow::Result<()> {
        ensure!(self.enabled(), "dependency management is disabled");
        Ok(())
    }

    fn set_operation(&self, id: &str, state: &str) {
        self.inner
            .operation_state
            .write()
            .unwrap()
            .insert(id.into(), state.into());
    }
}

fn spec(id: &str) -> anyhow::Result<&'static LibrarySpec> {
    LIBRARIES
        .iter()
        .find(|value| value.id == id)
        .context("dependency library not found")
}

async fn load_state(path: &Path) -> anyhow::Result<PersistedDependencies> {
    match tokio::fs::read(path).await {
        Ok(data) => Ok(serde_json::from_slice(&data)?),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            Ok(PersistedDependencies::default())
        }
        Err(error) => Err(error.into()),
    }
}

async fn persist_state(path: &Path, state: &PersistedDependencies) -> anyhow::Result<()> {
    write_json_synced(path, state).await
}

async fn apply_selection_paths(
    active: &Path,
    versions: &Path,
    spec: &LibrarySpec,
    sha256: Option<&str>,
) -> anyhow::Result<()> {
    tokio::fs::create_dir_all(active).await?;
    for load_name in spec.load_names {
        let target = active.join(load_name);
        if let Some(sha256) = sha256 {
            validate_sha(sha256)?;
            let source = versions.join(spec.id).join(sha256).join(spec.soname);
            ensure!(
                tokio::fs::metadata(&source).await.is_ok(),
                "dependency version not found"
            );
            let temporary = active.join(format!(".{load_name}.{}.part", Uuid::new_v4()));
            symlink(&source, &temporary)?;
            tokio::fs::rename(&temporary, &target).await?;
        } else if tokio::fs::symlink_metadata(&target).await.is_ok() {
            tokio::fs::remove_file(&target).await?;
        }
    }
    sync_dir(active).await
}

async fn cleanup_parts(versions: &Path) -> anyhow::Result<()> {
    let mut roots = match tokio::fs::read_dir(versions).await {
        Ok(value) => value,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error.into()),
    };
    while let Some(root) = roots.next_entry().await? {
        if !root.file_type().await?.is_dir() {
            continue;
        }
        let mut entries = tokio::fs::read_dir(root.path()).await?;
        while let Some(entry) = entries.next_entry().await? {
            if entry.file_type().await?.is_dir()
                && entry.file_name().to_string_lossy().starts_with('.')
            {
                tokio::fs::remove_dir_all(entry.path()).await?;
            }
        }
    }
    Ok(())
}

async fn read_version(path: &Path) -> anyhow::Result<DependencyVersion> {
    Ok(serde_json::from_slice(&tokio::fs::read(path).await?)?)
}

async fn write_json_synced(path: &Path, value: &impl Serialize) -> anyhow::Result<()> {
    let part = path.with_extension(format!("{}.part", Uuid::new_v4()));
    write_synced(&part, &serde_json::to_vec_pretty(value)?).await?;
    tokio::fs::rename(&part, path).await?;
    if let Some(parent) = path.parent() {
        sync_dir(parent).await?;
    }
    Ok(())
}

async fn write_synced(path: &Path, data: &[u8]) -> anyhow::Result<()> {
    use tokio::io::AsyncWriteExt;
    let mut file = tokio::fs::File::create(path).await?;
    file.write_all(data).await?;
    file.sync_all().await?;
    Ok(())
}

async fn sync_dir(path: &Path) -> anyhow::Result<()> {
    let path = path.to_owned();
    tokio::task::spawn_blocking(move || std::fs::File::open(path)?.sync_all()).await??;
    Ok(())
}

fn validate_sha(value: &str) -> anyhow::Result<()> {
    ensure!(
        value.len() == 64 && value.bytes().all(|byte| byte.is_ascii_hexdigit()),
        "invalid dependency SHA-256"
    );
    Ok(())
}

struct InspectedElf {
    soname: String,
    build_id: Option<String>,
    needed: Vec<String>,
}

fn inspect_elf(data: &[u8], expected_soname: &str) -> anyhow::Result<InspectedElf> {
    ensure!(data.len() >= 52, "invalid ELF header");
    ensure!(&data[0..4] == b"\x7fELF", "uploaded file is not ELF");
    ensure!(data[4] == 1, "dependency must be ELF32");
    ensure!(data[5] == 1, "dependency must be little-endian");
    ensure!(u16_at(data, 16)? == 3, "dependency must be ET_DYN");
    ensure!(u16_at(data, 18)? == 40, "dependency must target ARM");
    let phoff = u32_at(data, 28)? as usize;
    let phentsize = u16_at(data, 42)? as usize;
    let phnum = u16_at(data, 44)? as usize;
    ensure!(
        phentsize >= 32 && phnum <= 256,
        "invalid ELF program headers"
    );
    let mut loads = Vec::new();
    let mut dynamic = None;
    let mut notes = Vec::new();
    for index in 0..phnum {
        let offset = phoff
            .checked_add(index.checked_mul(phentsize).context("ELF overflow")?)
            .context("ELF overflow")?;
        ensure!(offset + 32 <= data.len(), "truncated ELF program header");
        let kind = u32_at(data, offset)?;
        let file_offset = u32_at(data, offset + 4)? as usize;
        let virtual_address = u32_at(data, offset + 8)? as usize;
        let file_size = u32_at(data, offset + 16)? as usize;
        let memory_size = u32_at(data, offset + 20)? as usize;
        ensure!(
            file_offset
                .checked_add(file_size)
                .is_some_and(|end| end <= data.len()),
            "truncated ELF segment"
        );
        match kind {
            1 => loads.push((virtual_address, memory_size, file_offset)),
            2 => dynamic = Some((file_offset, file_size)),
            4 => notes.push((file_offset, file_size)),
            _ => {}
        }
    }
    let (dynamic_offset, dynamic_size) = dynamic.context("ELF has no dynamic section")?;
    let mut string_vaddr = None;
    let mut string_size = None;
    let mut soname_offset = None;
    let mut needed_offsets = Vec::new();
    for offset in (dynamic_offset..dynamic_offset + dynamic_size).step_by(8) {
        if offset + 8 > data.len() {
            break;
        }
        let tag = i32_at(data, offset)?;
        let value = u32_at(data, offset + 4)? as usize;
        match tag {
            0 => break,
            1 => needed_offsets.push(value),
            5 => string_vaddr = Some(value),
            10 => string_size = Some(value),
            14 => soname_offset = Some(value),
            _ => {}
        }
    }
    let string_vaddr = string_vaddr.context("ELF has no dynamic string table")?;
    let string_file = loads
        .iter()
        .find_map(|(vaddr, size, offset)| {
            (string_vaddr >= *vaddr && string_vaddr < vaddr.saturating_add(*size))
                .then_some(offset + string_vaddr - vaddr)
        })
        .context("cannot map ELF dynamic string table")?;
    ensure!(
        string_file < data.len(),
        "ELF dynamic string table is out of range"
    );
    let string_end = string_file
        .saturating_add(string_size.unwrap_or(data.len() - string_file))
        .min(data.len());
    let string_table = &data[string_file..string_end];
    let soname = c_string(string_table, soname_offset.context("ELF has no SONAME")?)?;
    ensure!(
        soname == expected_soname,
        "dependency SONAME must be {expected_soname}, got {soname}"
    );
    let mut needed = Vec::new();
    for offset in needed_offsets {
        let value = c_string(string_table, offset)?;
        ensure!(
            !value.contains('/') && value.len() <= 128,
            "invalid ELF NEEDED entry"
        );
        needed.push(value);
    }
    let build_id = notes
        .into_iter()
        .find_map(|(offset, size)| parse_build_id(&data[offset..offset + size]));
    Ok(InspectedElf {
        soname,
        build_id,
        needed,
    })
}

fn parse_build_id(notes: &[u8]) -> Option<String> {
    let mut offset = 0usize;
    while offset + 12 <= notes.len() {
        let namesz = u32::from_le_bytes(notes[offset..offset + 4].try_into().ok()?) as usize;
        let descsz = u32::from_le_bytes(notes[offset + 4..offset + 8].try_into().ok()?) as usize;
        let kind = u32::from_le_bytes(notes[offset + 8..offset + 12].try_into().ok()?);
        offset += 12;
        let name_end = offset.checked_add(namesz)?;
        if name_end > notes.len() {
            return None;
        }
        let name = &notes[offset..name_end];
        offset = align4(name_end);
        let desc_end = offset.checked_add(descsz)?;
        if desc_end > notes.len() {
            return None;
        }
        if kind == 3 && name.starts_with(b"GNU") {
            return Some(hex(&notes[offset..desc_end]));
        }
        offset = align4(desc_end);
    }
    None
}

fn detect_rknn_version(data: &[u8]) -> Option<String> {
    let marker = b"librknnmrt version:";
    let start = data
        .windows(marker.len())
        .position(|value| value == marker)?;
    let tail = &data[start + marker.len()..];
    let length = tail
        .iter()
        .position(|byte| matches!(byte, 0 | b'\n' | b'\r'))
        .unwrap_or(tail.len())
        .min(160);
    let value = String::from_utf8_lossy(&tail[..length]).trim().to_owned();
    (!value.is_empty()).then_some(value)
}

fn c_string(table: &[u8], offset: usize) -> anyhow::Result<String> {
    ensure!(offset < table.len(), "ELF string offset is out of range");
    let end = table[offset..]
        .iter()
        .position(|byte| *byte == 0)
        .map(|value| offset + value)
        .context("unterminated ELF string")?;
    Ok(std::str::from_utf8(&table[offset..end])?.to_owned())
}

fn u16_at(data: &[u8], offset: usize) -> anyhow::Result<u16> {
    Ok(u16::from_le_bytes(
        data.get(offset..offset + 2)
            .context("truncated ELF")?
            .try_into()?,
    ))
}

fn u32_at(data: &[u8], offset: usize) -> anyhow::Result<u32> {
    Ok(u32::from_le_bytes(
        data.get(offset..offset + 4)
            .context("truncated ELF")?
            .try_into()?,
    ))
}

fn i32_at(data: &[u8], offset: usize) -> anyhow::Result<i32> {
    Ok(i32::from_le_bytes(
        data.get(offset..offset + 4)
            .context("truncated ELF")?
            .try_into()?,
    ))
}

fn align4(value: usize) -> usize {
    (value + 3) & !3
}

fn hex(data: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(data.len() * 2);
    for byte in data {
        output.push(HEX[(byte >> 4) as usize] as char);
        output.push(HEX[(byte & 0x0f) as usize] as char);
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    fn put16(data: &mut [u8], offset: usize, value: u16) {
        data[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
    }

    fn put32(data: &mut [u8], offset: usize, value: u32) {
        data[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn arm_shared_object() -> Vec<u8> {
        let mut data = vec![0_u8; 0x260];
        let length = data.len() as u32;
        data[0..7].copy_from_slice(b"\x7fELF\x01\x01\x01");
        put16(&mut data, 16, 3);
        put16(&mut data, 18, 40);
        put32(&mut data, 20, 1);
        put32(&mut data, 28, 52);
        put16(&mut data, 40, 52);
        put16(&mut data, 42, 32);
        put16(&mut data, 44, 3);
        put32(&mut data, 52, 1);
        put32(&mut data, 56, 0);
        put32(&mut data, 60, 0x1000);
        put32(&mut data, 68, length);
        put32(&mut data, 72, length);
        put32(&mut data, 84, 2);
        put32(&mut data, 88, 0x100);
        put32(&mut data, 92, 0x1100);
        put32(&mut data, 100, 40);
        put32(&mut data, 104, 40);
        put32(&mut data, 116, 4);
        put32(&mut data, 120, 0x180);
        put32(&mut data, 124, 0x1180);
        put32(&mut data, 132, 20);
        put32(&mut data, 136, 20);
        for (index, (tag, value)) in [(5, 0x1200), (10, 64), (14, 1), (1, 9), (0, 0)]
            .into_iter()
            .enumerate()
        {
            put32(&mut data, 0x100 + index * 8, tag);
            put32(&mut data, 0x104 + index * 8, value);
        }
        put32(&mut data, 0x180, 4);
        put32(&mut data, 0x184, 4);
        put32(&mut data, 0x188, 3);
        data[0x18c..0x190].copy_from_slice(b"GNU\0");
        data[0x190..0x194].copy_from_slice(&[1, 2, 3, 4]);
        data[0x200..0x21a].copy_from_slice(b"\0libx.so\0libc.so.0\0padding");
        data
    }

    #[test]
    fn rejects_non_elf_and_wrong_sha() {
        assert!(inspect_elf(b"not an elf", "libx.so").is_err());
        assert!(validate_sha("abc").is_err());
        assert!(validate_sha(&"a".repeat(64)).is_ok());
    }

    #[test]
    fn extracts_rknn_version() {
        assert_eq!(
            detect_rknn_version(b"xxlibrknnmrt version: 2.3.2 (build)\0yy").as_deref(),
            Some("2.3.2 (build)")
        );
    }

    #[test]
    fn inspects_arm_soname_needed_and_build_id() {
        let inspected = inspect_elf(&arm_shared_object(), "libx.so").unwrap();
        assert_eq!(inspected.soname, "libx.so");
        assert_eq!(inspected.needed, vec!["libc.so.0"]);
        assert_eq!(inspected.build_id.as_deref(), Some("01020304"));
        assert!(inspect_elf(&arm_shared_object(), "libother.so").is_err());
    }

    #[tokio::test]
    async fn recovery_reconciles_active_links_and_cleans_parts() {
        let temp = tempdir().unwrap();
        let config = DependencyConfig {
            enabled: true,
            root: temp.path().join("dependencies"),
            max_upload_bytes: 1024 * 1024,
        };
        let sha = "a".repeat(64);
        let version = config.root.join("versions/rknn-runtime").join(&sha);
        tokio::fs::create_dir_all(&version).await.unwrap();
        tokio::fs::write(version.join("librknnmrt.so"), b"fixture")
            .await
            .unwrap();
        let part = config.root.join("versions/rknn-runtime/.interrupted.part");
        tokio::fs::create_dir_all(&part).await.unwrap();
        let mut persisted = PersistedDependencies::default();
        persisted.libraries.insert(
            "rknn-runtime".into(),
            LibrarySelection {
                active: Some(sha.clone()),
                ..LibrarySelection::default()
            },
        );
        tokio::fs::create_dir_all(&config.root).await.unwrap();
        persist_state(&config.root.join("state.json"), &persisted)
            .await
            .unwrap();

        DependencyManager::recover(&config).await.unwrap();
        let link = tokio::fs::read_link(config.root.join("active/librknnmrt.so"))
            .await
            .unwrap();
        assert_eq!(link, version.join("librknnmrt.so"));
        assert!(tokio::fs::metadata(part).await.is_err());

        persisted.libraries.get_mut("rknn-runtime").unwrap().active = None;
        persist_state(&config.root.join("state.json"), &persisted)
            .await
            .unwrap();
        DependencyManager::recover(&config).await.unwrap();
        assert!(
            tokio::fs::symlink_metadata(config.root.join("active/librknnmrt.so"))
                .await
                .is_err()
        );
    }
}
