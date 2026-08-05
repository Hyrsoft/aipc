mod api;
mod config;
mod model;
mod store;
mod supervisor;

use anyhow::{Context, bail};
use clap::Parser;
use config::{DaemonConfig, WorkerConfig};
use model::PersistentState;
use std::path::PathBuf;
use store::StateStore;
use supervisor::spawn_supervisor;
use tokio::net::TcpListener;
use tracing::{info, warn};
use tracing_subscriber::EnvFilter;

#[derive(Debug, Parser)]
#[command(version, about = "RV1106 AIPC media worker supervisor")]
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

    let store = StateStore::new(&settings.data_dir);
    let initial = match store.load().await? {
        Some(state) => state,
        None => PersistentState::new(load_seed(&settings.seed_config).await?),
    };
    let supervisor = spawn_supervisor(settings.clone(), initial).await;
    let app = api::router(supervisor.clone(), &settings.web_dir);
    let listener = TcpListener::bind(&settings.bind)
        .await
        .with_context(|| format!("bind HTTP server at {}", settings.bind))?;
    info!(bind = %settings.bind, "aipc daemon ready (trusted LAN, authentication disabled)");

    axum::serve(listener, app)
        .with_graceful_shutdown(shutdown_signal())
        .await?;
    info!("HTTP shutdown requested; stopping media worker");
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
