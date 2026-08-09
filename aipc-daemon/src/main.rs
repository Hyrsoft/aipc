mod ai;
mod ai_manager;
mod ai_results;
mod api;
mod config;
mod dependencies;
mod model;
mod preview;
mod recording;
mod rtsp;
mod source;
mod store;
mod supervisor;
mod watchdog;
mod webrtc;

use anyhow::{Context, bail};
use clap::Parser;
use config::{DaemonConfig, WorkerConfig};
use model::PersistentState;
use source::SourceManager;
use std::path::PathBuf;
use store::StateStore;
use supervisor::spawn_supervisor;
use tokio::net::TcpListener;
use tracing::{info, warn};
use tracing_subscriber::EnvFilter;

#[derive(Debug, Parser)]
#[command(version, about = "AIPC media worker supervisor")]
struct Args {
    #[arg(long)]
    config: Option<PathBuf>,
    #[arg(long)]
    bind: Option<String>,
    #[arg(long)]
    worker: Option<PathBuf>,
    #[arg(long)]
    web_dir: Option<PathBuf>,
    #[arg(long)]
    no_autostart: bool,
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()))
        .with_writer(std::io::stderr)
        .init();
    let args = Args::parse();
    let executable = std::env::current_exe().context("resolve daemon executable")?;
    let executable_dir = executable
        .parent()
        .context("daemon executable has no parent directory")?;
    let mut settings = DaemonConfig::load(args.config.as_deref(), executable_dir).await?;
    if let Some(bind) = args.bind {
        settings.bind = bind;
    }
    if let Some(worker) = args.worker {
        settings.worker_path = absolute_from(executable_dir, worker);
    }
    if let Some(web_dir) = args.web_dir {
        settings.web_dir = absolute_from(executable_dir, web_dir);
    }
    if args.no_autostart {
        settings.autostart = false;
    }
    settings.preview.broadcast_capacity = settings
        .preview
        .broadcast_capacity
        .max(settings.recording.queue_capacity);

    let store = StateStore::new(&settings.data_dir);
    let mut initial = match store.load().await? {
        Some(state) => state,
        None => PersistentState::new(load_seed(&settings.seed_config).await?),
    };
    // The active Lua manifest is the sole authority for the AI VPSS channel.
    // Always boot the media worker with the side channel disabled; AiManager
    // restores a persisted project online after FD 6 becomes ready.
    for config in [
        initial.desired.as_mut(),
        initial.active.as_mut(),
        initial.pending.as_mut(),
        initial.last_good.as_mut(),
    ]
    .into_iter()
    .flatten()
    {
        config.ai_input.enabled = false;
    }
    dependencies::DependencyManager::recover(&settings.dependencies).await?;
    let supervisor = spawn_supervisor(settings.clone(), initial).await;
    let sources = SourceManager::new(
        settings.input.clone(),
        supervisor.preview.clone(),
        supervisor.ai.clone(),
        supervisor.events.clone(),
    )?;
    if sources.enabled() {
        sources.start_active().await?;
    }
    let ai = ai_manager::AiManager::new(
        settings.ai.clone(),
        &settings.data_dir,
        supervisor.ai.clone(),
        supervisor.events.clone(),
    )
    .await?;
    ai.start_persisted();
    let dependencies = dependencies::DependencyManager::new(
        settings.dependencies.clone(),
        executable_dir,
        settings.worker_path.clone(),
        settings.ai.worker_path.clone(),
        supervisor.clone(),
        ai.clone(),
        supervisor.events.clone(),
    )
    .await?;
    let recording = recording::RecordingManager::new(
        settings.recording.clone(),
        &settings.data_dir,
        supervisor.preview.clone(),
        supervisor.events.clone(),
    )
    .await?;
    let rtsp = rtsp::RtspServer::start(
        settings.rtsp.clone(),
        supervisor.preview.clone(),
        supervisor.events.clone(),
    )
    .await?;
    let webrtc = webrtc::WebRtcServer::start(
        settings.webrtc.clone(),
        supervisor.preview.clone(),
        supervisor.events.clone(),
        Some(ai.clone()),
    )
    .await?;
    let app = api::router(
        supervisor.clone(),
        recording.clone(),
        rtsp.clone(),
        webrtc.clone(),
        ai.clone(),
        dependencies,
        sources.clone(),
        settings.ui.clone(),
        &settings.web_dir,
    );
    let listener = TcpListener::bind(&settings.bind)
        .await
        .with_context(|| format!("bind HTTP server at {}", settings.bind))?;
    let _watchdog = watchdog::Watchdog::start(&settings.watchdog)?;
    info!(bind = %settings.bind, "aipc daemon ready (trusted LAN, authentication disabled)");

    axum::serve(
        listener,
        app.into_make_service_with_connect_info::<std::net::SocketAddr>(),
    )
    .with_graceful_shutdown(shutdown_signal())
    .await?;
    info!("HTTP shutdown requested; stopping media worker");
    recording.shutdown().await;
    rtsp.shutdown().await;
    webrtc.shutdown().await;
    sources.shutdown().await;
    ai.shutdown().await;
    supervisor.shutdown().await;
    Ok(())
}

async fn load_seed(path: &std::path::Path) -> anyhow::Result<WorkerConfig> {
    let data = tokio::fs::read(path)
        .await
        .with_context(|| format!("read seed worker config {}", path.display()))?;
    let config: WorkerConfig = serde_json::from_slice(&data)
        .with_context(|| format!("parse seed worker config {}", path.display()))?;
    let errors = config.validate();
    if !errors.is_empty() {
        bail!("invalid seed worker config: {}", errors.join("; "));
    }
    Ok(config)
}

fn absolute_from(base: &std::path::Path, value: PathBuf) -> PathBuf {
    if value.is_absolute() {
        value
    } else {
        base.join(value)
    }
}

async fn shutdown_signal() {
    let ctrl_c = async {
        if let Err(error) = tokio::signal::ctrl_c().await {
            warn!(%error, "failed to install Ctrl-C handler");
        }
    };
    #[cfg(unix)]
    let terminate = async {
        match tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()) {
            Ok(mut signal) => {
                signal.recv().await;
            }
            Err(error) => warn!(%error, "failed to install SIGTERM handler"),
        }
    };
    #[cfg(not(unix))]
    let terminate = std::future::pending::<()>();
    tokio::select! {
        _ = ctrl_c => {},
        _ = terminate => {},
    }
}
