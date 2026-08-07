use super::{ProcessMessage, SupervisorError};
use crate::config::WorkerConfig;
use nix::sys::signal::{Signal, kill};
use nix::unistd::Pid;
use std::os::unix::process::ExitStatusExt;
use std::path::Path;
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::sync::mpsc;
use tracing::warn;
use uuid::Uuid;

pub(super) async fn write_worker_config(path: &Path, config: &WorkerConfig) -> anyhow::Result<()> {
    let data = serde_json::to_vec_pretty(config)?;
    tokio::fs::write(path, data).await?;
    Ok(())
}

pub(super) fn spawn_stdout_reader(
    stdout: tokio::process::ChildStdout,
    generation: String,
    sender: mpsc::Sender<ProcessMessage>,
) {
    tokio::spawn(async move {
        let mut lines = BufReader::new(stdout).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            let message = match serde_json::from_str(&line) {
                Ok(value) => ProcessMessage::Event {
                    generation: generation.clone(),
                    value,
                },
                Err(_) => ProcessMessage::InvalidEvent {
                    generation: generation.clone(),
                    line,
                },
            };
            if sender.send(message).await.is_err() {
                break;
            }
        }
    });
}

pub(super) fn spawn_stderr_reader(
    stderr: tokio::process::ChildStderr,
    generation: String,
    sender: mpsc::Sender<ProcessMessage>,
) {
    tokio::spawn(async move {
        let mut lines = BufReader::new(stderr).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            if sender
                .send(ProcessMessage::Stderr {
                    generation: generation.clone(),
                    line,
                })
                .await
                .is_err()
            {
                break;
            }
        }
    });
}

pub(super) fn spawn_waiter(
    mut child: tokio::process::Child,
    generation: String,
    sender: mpsc::Sender<ProcessMessage>,
) {
    tokio::spawn(async move {
        match child.wait().await {
            Ok(status) => {
                let _ = sender
                    .send(ProcessMessage::Exited {
                        generation,
                        code: status.code(),
                        signal: status.signal(),
                    })
                    .await;
            }
            Err(error) => warn!(%error, "failed waiting for worker process"),
        }
    });
}

pub(super) fn signal_process(pid: u32, signal: Signal) -> Result<(), SupervisorError> {
    kill(Pid::from_raw(pid as i32), signal)
        .map_err(|error| SupervisorError::Operation(error.to_string()))
}

pub(super) fn new_generation() -> String {
    Uuid::new_v4().to_string()
}
