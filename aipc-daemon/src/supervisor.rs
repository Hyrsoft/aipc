use crate::config::{DaemonConfig, WorkerConfig};
use crate::model::{DaemonStatus, LogEntry, PersistentState, ProcessState, ServerEvent, now_ms};
use crate::preview::{PreviewHub, StreamInfo, read_video_ipc};
use crate::store::StateStore;
use nix::sys::signal::{Signal, kill};
use nix::unistd::Pid;
use serde::Serialize;
use serde_json::{Value, json};
use std::collections::VecDeque;
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream as StdUnixStream;
use std::os::unix::process::ExitStatusExt;
use std::path::Path;
use std::process::Stdio;
use std::sync::Arc;
use std::time::Duration;
use thiserror::Error;
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::Command;
use tokio::sync::{Mutex, RwLock, broadcast, mpsc, oneshot, watch};
use tokio::time::Instant;
use tracing::{error, info, warn};
use uuid::Uuid;

const LOG_CAPACITY: usize = 200;

#[derive(Debug, Error)]
pub enum SupervisorError {
    #[error("worker is already in transition")]
    Conflict,
    #[error("worker is already running")]
    AlreadyRunning,
    #[error("worker is already stopped")]
    AlreadyStopped,
    #[error("configuration rejected: {0}")]
    InvalidConfig(String),
    #[error("worker operation failed: {0}")]
    Operation(String),
}

#[derive(Debug, Clone, Serialize)]
pub struct ActionAccepted {
    pub generation: Option<String>,
    pub action: String,
}

#[derive(Clone)]
pub struct SupervisorHandle {
    commands: mpsc::Sender<SupervisorCommand>,
    pub status: watch::Receiver<DaemonStatus>,
    pub events: broadcast::Sender<ServerEvent>,
    pub persistent: Arc<RwLock<PersistentState>>,
    pub logs: Arc<Mutex<VecDeque<LogEntry>>>,
    pub preview: PreviewHub,
}

impl SupervisorHandle {
    pub async fn start(&self) -> Result<ActionAccepted, SupervisorError> {
        self.request(|reply| SupervisorCommand::Start { reply })
            .await
    }

    pub async fn stop(&self) -> Result<ActionAccepted, SupervisorError> {
        self.request(|reply| SupervisorCommand::Stop { reply })
            .await
    }

    pub async fn restart(&self) -> Result<ActionAccepted, SupervisorError> {
        self.request(|reply| SupervisorCommand::Restart { reply })
            .await
    }

    pub async fn apply(&self, config: WorkerConfig) -> Result<ActionAccepted, SupervisorError> {
        self.request(|reply| SupervisorCommand::Apply { config, reply })
            .await
    }

    pub async fn shutdown(&self) {
        let (reply, receiver) = oneshot::channel();
        let _ = self
            .commands
            .send(SupervisorCommand::Shutdown { reply })
            .await;
        let _ = receiver.await;
        let mut status = self.status.clone();
        let _ = tokio::time::timeout(Duration::from_secs(7), async {
            while status.borrow().pid.is_some() {
                if status.changed().await.is_err() {
                    break;
                }
            }
        })
        .await;
    }

    async fn request<F>(&self, make: F) -> Result<ActionAccepted, SupervisorError>
    where
        F: FnOnce(oneshot::Sender<Result<ActionAccepted, SupervisorError>>) -> SupervisorCommand,
    {
        let (reply, receiver) = oneshot::channel();
        self.commands
            .send(make(reply))
            .await
            .map_err(|_| SupervisorError::Operation("supervisor task stopped".into()))?;
        receiver
            .await
            .map_err(|_| SupervisorError::Operation("supervisor reply dropped".into()))?
    }
}

pub async fn spawn_supervisor(
    settings: DaemonConfig,
    initial: PersistentState,
) -> SupervisorHandle {
    let (command_tx, command_rx) = mpsc::channel(32);
    let (process_tx, process_rx) = mpsc::channel(256);
    let (status_tx, status_rx) = watch::channel(DaemonStatus {
        updated_at_ms: now_ms(),
        ..DaemonStatus::default()
    });
    let (events, _) = broadcast::channel(256);
    let persistent = Arc::new(RwLock::new(initial));
    let logs = Arc::new(Mutex::new(VecDeque::with_capacity(LOG_CAPACITY)));
    let preview = PreviewHub::new(settings.preview.clone(), events.clone());
    let handle = SupervisorHandle {
        commands: command_tx,
        status: status_rx,
        events: events.clone(),
        persistent: persistent.clone(),
        logs: logs.clone(),
        preview: preview.clone(),
    };
    let mut actor = SupervisorActor {
        store: StateStore::new(&settings.data_dir),
        settings,
        command_rx,
        process_tx,
        process_rx,
        status_tx,
        events,
        persistent,
        logs,
        preview,
        running: None,
        after_stop: None,
        restart_times: VecDeque::new(),
        total_starts: 0,
        backoff_deadline: None,
        shutting_down: false,
    };
    tokio::spawn(async move { actor.run().await });
    handle
}

enum SupervisorCommand {
    Start { reply: Reply },
    Stop { reply: Reply },
    Restart { reply: Reply },
    Apply { config: WorkerConfig, reply: Reply },
    Shutdown { reply: oneshot::Sender<()> },
}

type Reply = oneshot::Sender<Result<ActionAccepted, SupervisorError>>;

enum ProcessMessage {
    Event {
        generation: String,
        value: Value,
    },
    InvalidEvent {
        generation: String,
        line: String,
    },
    Stderr {
        generation: String,
        line: String,
    },
    Exited {
        generation: String,
        code: Option<i32>,
        signal: Option<i32>,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum StartReason {
    Normal,
    Apply,
    Rollback,
    Restart,
    AutoRestart,
}

struct PendingStart {
    config: WorkerConfig,
    generation: String,
    reason: StartReason,
}

struct RunningProcess {
    pid: u32,
    config: WorkerConfig,
    generation: String,
    reason: StartReason,
    video_ready: bool,
    audio_ready: bool,
    ready: bool,
    start_deadline: Instant,
    stop_deadline: Option<Instant>,
    manual_stop: bool,
    startup_timed_out: bool,
}

struct SupervisorActor {
    settings: DaemonConfig,
    store: StateStore,
    command_rx: mpsc::Receiver<SupervisorCommand>,
    process_tx: mpsc::Sender<ProcessMessage>,
    process_rx: mpsc::Receiver<ProcessMessage>,
    status_tx: watch::Sender<DaemonStatus>,
    events: broadcast::Sender<ServerEvent>,
    persistent: Arc<RwLock<PersistentState>>,
    logs: Arc<Mutex<VecDeque<LogEntry>>>,
    preview: PreviewHub,
    running: Option<RunningProcess>,
    after_stop: Option<PendingStart>,
    restart_times: VecDeque<Instant>,
    total_starts: usize,
    backoff_deadline: Option<(Instant, PendingStart)>,
    shutting_down: bool,
}

impl SupervisorActor {
    async fn run(&mut self) {
        if let Err(error) = self.persist().await {
            warn!(%error, "failed to persist initial state");
        }
        if self.settings.autostart {
            if let Some(config) = self.startup_config().await {
                let generation = new_generation();
                if let Err(error) = self
                    .spawn_process(PendingStart {
                        config,
                        generation,
                        reason: StartReason::Normal,
                    })
                    .await
                {
                    self.fail(error).await;
                }
            }
        }

        let mut tick = tokio::time::interval(Duration::from_millis(200));
        loop {
            tokio::select! {
                Some(command) = self.command_rx.recv() => self.handle_command(command).await,
                Some(message) = self.process_rx.recv() => self.handle_process_message(message).await,
                _ = tick.tick() => self.handle_tick().await,
                else => break,
            }
            if self.shutting_down && self.running.is_none() {
                break;
            }
        }
    }

    async fn handle_command(&mut self, command: SupervisorCommand) {
        match command {
            SupervisorCommand::Start { reply } => {
                let result = self.start_command().await;
                let _ = reply.send(result);
            }
            SupervisorCommand::Stop { reply } => {
                let result = self.stop_command(false).await;
                let _ = reply.send(result);
            }
            SupervisorCommand::Restart { reply } => {
                let result = self.restart_command().await;
                let _ = reply.send(result);
            }
            SupervisorCommand::Apply { config, reply } => {
                let result = self.apply_command(config).await;
                let _ = reply.send(result);
            }
            SupervisorCommand::Shutdown { reply } => {
                self.shutting_down = true;
                self.after_stop = None;
                self.backoff_deadline = None;
                if self.running.is_some() {
                    let _ = self.stop_command(true).await;
                }
                let _ = reply.send(());
            }
        }
    }

    async fn start_command(&mut self) -> Result<ActionAccepted, SupervisorError> {
        self.ensure_idle()?;
        if self.running.is_some() {
            return Err(SupervisorError::AlreadyRunning);
        }
        let config = self
            .startup_config()
            .await
            .ok_or_else(|| SupervisorError::Operation("no worker configuration".into()))?;
        let generation = new_generation();
        self.restart_times.clear();
        self.backoff_deadline = None;
        self.spawn_process(PendingStart {
            config,
            generation: generation.clone(),
            reason: StartReason::Normal,
        })
        .await
        .map_err(SupervisorError::Operation)?;
        Ok(ActionAccepted {
            generation: Some(generation),
            action: "start".into(),
        })
    }

    async fn stop_command(&mut self, shutdown: bool) -> Result<ActionAccepted, SupervisorError> {
        if self.running.is_none() {
            if self.backoff_deadline.take().is_some() {
                self.update_status(|status| {
                    status.state = ProcessState::Stopped;
                    status.stage = Some("manual_stop".into());
                });
                return Ok(ActionAccepted {
                    generation: None,
                    action: "stop".into(),
                });
            }
            return Err(SupervisorError::AlreadyStopped);
        }
        self.after_stop = None;
        self.backoff_deadline = None;
        if let Some(running) = self.running.as_mut() {
            running.manual_stop = true;
            running.stop_deadline =
                Some(Instant::now() + Duration::from_millis(self.settings.stop_timeout_ms));
            signal_process(running.pid, Signal::SIGTERM)?;
        }
        self.update_status(|status| {
            status.state = ProcessState::Stopping;
            status.stage = Some(if shutdown { "shutdown" } else { "manual_stop" }.into());
        });
        Ok(ActionAccepted {
            generation: self.running.as_ref().map(|item| item.generation.clone()),
            action: "stop".into(),
        })
    }

    async fn restart_command(&mut self) -> Result<ActionAccepted, SupervisorError> {
        self.ensure_idle()?;
        let config = self
            .running
            .as_ref()
            .map(|item| item.config.clone())
            .or(self.startup_config().await)
            .ok_or_else(|| SupervisorError::Operation("no worker configuration".into()))?;
        let generation = new_generation();
        self.restart_times.clear();
        self.backoff_deadline = None;
        let pending = PendingStart {
            config,
            generation: generation.clone(),
            reason: StartReason::Restart,
        };
        if self.running.is_some() {
            self.after_stop = Some(pending);
            self.begin_transition_stop("restart")?;
        } else {
            self.spawn_process(pending)
                .await
                .map_err(SupervisorError::Operation)?;
        }
        Ok(ActionAccepted {
            generation: Some(generation),
            action: "restart".into(),
        })
    }

    async fn apply_command(
        &mut self,
        mut config: WorkerConfig,
    ) -> Result<ActionAccepted, SupervisorError> {
        self.ensure_idle()?;
        let errors = config.validate();
        if !errors.is_empty() {
            return Err(SupervisorError::InvalidConfig(errors.join("; ")));
        }
        let generation = new_generation();
        config.runtime.generation = generation.clone();
        self.validate_with_worker(&config, &generation).await?;
        self.backoff_deadline = None;
        {
            let mut state = self.persistent.write().await;
            state.desired = Some(config.clone());
            state.pending = Some(config.clone());
            state.last_error = None;
        }
        self.persist().await.map_err(|error| {
            SupervisorError::Operation(format!("persist pending config: {error}"))
        })?;
        let pending = PendingStart {
            config,
            generation: generation.clone(),
            reason: StartReason::Apply,
        };
        if self.running.is_some() {
            self.after_stop = Some(pending);
            self.begin_transition_stop("apply_config")?;
        } else {
            self.spawn_process(pending)
                .await
                .map_err(SupervisorError::Operation)?;
        }
        Ok(ActionAccepted {
            generation: Some(generation),
            action: "apply_config".into(),
        })
    }

    fn ensure_idle(&self) -> Result<(), SupervisorError> {
        if self.status_tx.borrow().state.is_transitioning() || self.after_stop.is_some() {
            Err(SupervisorError::Conflict)
        } else {
            Ok(())
        }
    }

    fn begin_transition_stop(&mut self, stage: &str) -> Result<(), SupervisorError> {
        let running = self
            .running
            .as_mut()
            .ok_or(SupervisorError::AlreadyStopped)?;
        running.manual_stop = true;
        running.stop_deadline =
            Some(Instant::now() + Duration::from_millis(self.settings.stop_timeout_ms));
        signal_process(running.pid, Signal::SIGTERM)?;
        self.update_status(|status| {
            status.state = ProcessState::Stopping;
            status.stage = Some(stage.into());
        });
        Ok(())
    }

    async fn spawn_process(&mut self, mut pending: PendingStart) -> Result<(), String> {
        tokio::fs::create_dir_all(&self.settings.runtime_dir)
            .await
            .map_err(|error| error.to_string())?;
        pending.config.runtime.generation = pending.generation.clone();
        let config_path = self
            .settings
            .runtime_dir
            .join(format!("media-worker-{}.json", pending.generation));
        write_worker_config(&config_path, &pending.config)
            .await
            .map_err(|error| error.to_string())?;

        let mut command = Command::new(&self.settings.worker_path);
        let ipc_pair = if self.preview.enabled() {
            let (parent, child) = StdUnixStream::pair().map_err(|error| error.to_string())?;
            parent
                .set_nonblocking(true)
                .map_err(|error| error.to_string())?;
            Some((parent, child))
        } else {
            None
        };
        let ipc_child_fd = ipc_pair.as_ref().map(|(_, child)| child.as_raw_fd());
        command
            .arg("--config")
            .arg(&config_path)
            .arg("--generation")
            .arg(&pending.generation)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(false);
        if ipc_child_fd.is_some() {
            command.arg("--video-ipc-fd").arg("3");
        }
        unsafe {
            command.pre_exec(move || {
                if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) != 0 {
                    return Err(std::io::Error::last_os_error());
                }
                if let Some(source_fd) = ipc_child_fd {
                    if source_fd == 3 {
                        let flags = libc::fcntl(3, libc::F_GETFD);
                        if flags < 0 || libc::fcntl(3, libc::F_SETFD, flags & !libc::FD_CLOEXEC) < 0
                        {
                            return Err(std::io::Error::last_os_error());
                        }
                    } else if libc::dup2(source_fd, 3) < 0 {
                        return Err(std::io::Error::last_os_error());
                    }
                }
                Ok(())
            });
        }
        let mut child = command.spawn().map_err(|error| error.to_string())?;
        if let Some((parent, child_side)) = ipc_pair {
            drop(child_side);
            let info = StreamInfo {
                generation: pending.generation.clone(),
                codec: "h264",
                format: "annexb",
                width: pending.config.video.width,
                height: pending.config.video.height,
                fps: pending.config.video.fps,
            };
            self.preview.begin_generation(info.clone());
            let stream =
                tokio::net::UnixStream::from_std(parent).map_err(|error| error.to_string())?;
            tokio::spawn(read_video_ipc(stream, self.preview.clone(), info));
        }
        let pid = child
            .id()
            .ok_or_else(|| "spawned worker has no PID".to_string())?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| "worker stdout was not piped".to_string())?;
        let stderr = child
            .stderr
            .take()
            .ok_or_else(|| "worker stderr was not piped".to_string())?;
        spawn_stdout_reader(stdout, pending.generation.clone(), self.process_tx.clone());
        spawn_stderr_reader(stderr, pending.generation.clone(), self.process_tx.clone());
        spawn_waiter(child, pending.generation.clone(), self.process_tx.clone());

        let state = if pending.reason == StartReason::Rollback {
            ProcessState::RollingBack
        } else {
            ProcessState::Starting
        };
        let clear_error = pending.reason != StartReason::Rollback;
        self.running = Some(RunningProcess {
            pid,
            config: pending.config,
            generation: pending.generation.clone(),
            reason: pending.reason,
            video_ready: false,
            audio_ready: false,
            ready: false,
            start_deadline: Instant::now()
                + Duration::from_millis(self.settings.startup_timeout_ms),
            stop_deadline: None,
            manual_stop: false,
            startup_timed_out: false,
        });
        self.total_starts += 1;
        let restart_count = self.total_starts.saturating_sub(1);
        self.update_status(|status| {
            status.state = state;
            status.pid = Some(pid);
            status.generation = Some(pending.generation.clone());
            status.stage = Some("spawned".into());
            status.started_at_ms = Some(now_ms());
            status.video_ready = false;
            status.audio_ready = false;
            status.metrics = None;
            status.restart_count = restart_count;
            if clear_error {
                status.last_error = None;
            }
        });
        self.emit("supervisor", json!({"action": "spawned", "pid": pid}));
        info!(pid, generation = %pending.generation, "media worker spawned");
        Ok(())
    }

    async fn handle_process_message(&mut self, message: ProcessMessage) {
        match message {
            ProcessMessage::Event { generation, value } => {
                if !self.is_current(&generation) {
                    return;
                }
                self.emit("worker_event", value.clone());
                self.apply_worker_event(value).await;
            }
            ProcessMessage::InvalidEvent { generation, line } => {
                if self.is_current(&generation) {
                    self.push_log("stdout", format!("invalid JSONL: {line}"))
                        .await;
                }
            }
            ProcessMessage::Stderr { generation, line } => {
                if self.is_current(&generation) {
                    self.push_log("stderr", line).await;
                }
            }
            ProcessMessage::Exited {
                generation,
                code,
                signal,
            } => self.handle_exit(generation, code, signal).await,
        }
    }

    async fn apply_worker_event(&mut self, value: Value) {
        let event = value.get("event").and_then(Value::as_str).unwrap_or("");
        match event {
            "BootProgress" => {
                let stage = value
                    .get("stage")
                    .and_then(Value::as_str)
                    .map(str::to_owned);
                self.update_status(|status| status.stage = stage);
            }
            "StreamReady" => {
                let media = value.get("media").and_then(Value::as_str).unwrap_or("");
                let mut became_ready = false;
                if let Some(running) = self.running.as_mut() {
                    if media == "video" {
                        running.video_ready = true;
                    } else if media == "audio" {
                        running.audio_ready = true;
                    }
                    let ready = running.video_ready
                        && (!running.config.audio.enabled || running.audio_ready);
                    if ready && !running.ready {
                        running.ready = true;
                        became_ready = true;
                    }
                }
                self.update_ready_status();
                if became_ready {
                    self.mark_running().await;
                }
            }
            "Metrics" => self.update_status(|status| status.metrics = Some(value)),
            "FatalError" => {
                let message = value
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("worker fatal error")
                    .to_owned();
                self.update_status(|status| status.last_error = Some(message));
            }
            _ => {}
        }
    }

    async fn mark_running(&mut self) {
        let Some(running) = self.running.as_ref() else {
            return;
        };
        let config = running.config.clone();
        let reason = running.reason;
        let audio_ready = running.audio_ready;
        {
            let mut state = self.persistent.write().await;
            state.active = Some(config.clone());
            state.last_good = Some(config);
            state.pending = None;
            if reason != StartReason::Rollback {
                state.last_error = None;
            }
        }
        if let Err(error) = self.persist().await {
            error!(%error, "failed to persist running state");
        }
        self.update_status(|status| {
            status.state = ProcessState::Running;
            status.stage = Some("ready".into());
            status.video_ready = true;
            status.audio_ready = audio_ready;
        });
        self.restart_times.clear();
        self.emit("supervisor", json!({"action": "running"}));
    }

    async fn handle_exit(&mut self, generation: String, code: Option<i32>, signal: Option<i32>) {
        if !self.is_current(&generation) {
            return;
        }
        let running = self.running.take().unwrap();
        self.preview.stop_generation(&generation);
        self.emit(
            "supervisor",
            json!({"action": "exited", "code": code, "signal": signal}),
        );
        self.update_status(|status| {
            status.pid = None;
            status.video_ready = false;
            status.audio_ready = false;
            status.metrics = None;
        });

        if let Some(pending) = self.after_stop.take() {
            if let Err(error) = self.spawn_process(pending).await {
                self.fail_or_rollback(error).await;
            }
            return;
        }
        if running.startup_timed_out && running.reason == StartReason::Apply {
            self.fail_or_rollback("candidate startup timed out".into())
                .await;
            return;
        }
        if running.startup_timed_out && running.reason == StartReason::Rollback {
            self.fail("rollback startup timed out".into()).await;
            return;
        }
        if self.shutting_down || running.manual_stop {
            self.update_status(|status| {
                status.state = ProcessState::Stopped;
                status.stage = Some("stopped".into());
            });
            return;
        }
        if running.reason == StartReason::Apply && !running.ready {
            self.fail_or_rollback(format!("candidate exited with code {code:?}"))
                .await;
            return;
        }
        self.schedule_restart(running.config).await;
    }

    async fn handle_tick(&mut self) {
        let now = Instant::now();
        let mut startup_timeout = None;
        if let Some(running) = self.running.as_mut() {
            if !running.ready && now >= running.start_deadline && running.stop_deadline.is_none() {
                let reason = running.reason;
                let error = "worker startup timed out".to_string();
                running.startup_timed_out = true;
                running.manual_stop = reason != StartReason::AutoRestart;
                running.stop_deadline =
                    Some(now + Duration::from_millis(self.settings.stop_timeout_ms));
                let _ = signal_process(running.pid, Signal::SIGTERM);
                let pid = running.pid;
                startup_timeout = Some((pid, error));
            }
            if let Some(deadline) = running.stop_deadline {
                if now >= deadline {
                    warn!(pid = running.pid, "worker did not stop; sending SIGKILL");
                    let _ = signal_process(running.pid, Signal::SIGKILL);
                    running.stop_deadline = None;
                }
            }
        }
        if let Some((pid, error)) = startup_timeout {
            self.update_status(|status| status.last_error = Some(error));
            warn!(pid, "worker startup timed out; requesting stop");
        }
        if self.running.is_none() {
            if let Some((deadline, _)) = &self.backoff_deadline {
                if now >= *deadline {
                    let (_, pending) = self.backoff_deadline.take().unwrap();
                    if let Err(error) = self.spawn_process(pending).await {
                        self.fail(error).await;
                    }
                }
            }
        }
    }

    async fn fail_or_rollback(&mut self, message: String) {
        let rollback = self.persistent.read().await.last_good.clone();
        {
            let mut state = self.persistent.write().await;
            state.pending = None;
            state.last_error = Some(message.clone());
        }
        let _ = self.persist().await;
        if let Some(config) = rollback {
            self.update_status(|status| {
                status.state = ProcessState::RollingBack;
                status.last_error = Some(message.clone());
                status.stage = Some("rollback".into());
            });
            let pending = PendingStart {
                config,
                generation: new_generation(),
                reason: StartReason::Rollback,
            };
            if self.running.is_some() {
                self.after_stop = Some(pending);
            } else if let Err(error) = self.spawn_process(pending).await {
                self.fail(error).await;
            }
        } else {
            self.fail(message).await;
        }
    }

    async fn schedule_restart(&mut self, config: WorkerConfig) {
        let now = Instant::now();
        let window = Duration::from_secs(self.settings.restart_window_sec);
        while self
            .restart_times
            .front()
            .is_some_and(|time| now.duration_since(*time) > window)
        {
            self.restart_times.pop_front();
        }
        if self.restart_times.len() >= self.settings.max_restarts {
            self.fail("automatic restart limit exceeded".into()).await;
            return;
        }
        self.restart_times.push_back(now);
        let delays = [1_u64, 2, 4, 8, 16, 30];
        let delay = delays[(self.restart_times.len() - 1).min(delays.len() - 1)];
        let generation = new_generation();
        self.backoff_deadline = Some((
            now + Duration::from_secs(delay),
            PendingStart {
                config,
                generation: generation.clone(),
                reason: StartReason::AutoRestart,
            },
        ));
        self.update_status(|status| {
            status.state = ProcessState::Backoff;
            status.stage = Some(format!("restart_in_{delay}s"));
            status.generation = Some(generation);
        });
    }

    async fn fail(&mut self, message: String) {
        {
            let mut state = self.persistent.write().await;
            state.last_error = Some(message.clone());
        }
        let _ = self.persist().await;
        self.update_status(|status| {
            status.state = ProcessState::Failed;
            status.last_error = Some(message.clone());
            status.stage = Some("failed".into());
        });
        self.emit(
            "supervisor",
            json!({"action": "failed", "message": message}),
        );
    }

    async fn validate_with_worker(
        &self,
        config: &WorkerConfig,
        generation: &str,
    ) -> Result<(), SupervisorError> {
        tokio::fs::create_dir_all(&self.settings.runtime_dir)
            .await
            .map_err(|error| SupervisorError::Operation(error.to_string()))?;
        let path = self
            .settings
            .runtime_dir
            .join(format!("validate-{generation}.json"));
        write_worker_config(&path, config)
            .await
            .map_err(|error| SupervisorError::Operation(error.to_string()))?;
        let result = tokio::time::timeout(
            Duration::from_secs(3),
            Command::new(&self.settings.worker_path)
                .arg("--config")
                .arg(&path)
                .arg("--validate-only")
                .output(),
        )
        .await
        .map_err(|_| SupervisorError::Operation("worker validation timed out".into()))?
        .map_err(|error| SupervisorError::Operation(error.to_string()))?;
        let _ = tokio::fs::remove_file(path).await;
        if result.status.success() {
            Ok(())
        } else {
            Err(SupervisorError::InvalidConfig(
                String::from_utf8_lossy(&result.stdout).into_owned(),
            ))
        }
    }

    async fn startup_config(&self) -> Option<WorkerConfig> {
        let state = self.persistent.read().await;
        state.last_good.clone().or_else(|| state.desired.clone())
    }

    fn is_current(&self, generation: &str) -> bool {
        self.running
            .as_ref()
            .is_some_and(|running| running.generation == generation)
    }

    fn update_ready_status(&self) {
        if let Some(running) = self.running.as_ref() {
            let video_ready = running.video_ready;
            let audio_ready = running.audio_ready;
            self.update_status(|status| {
                status.video_ready = video_ready;
                status.audio_ready = audio_ready;
            });
        }
    }

    fn update_status<F>(&self, update: F)
    where
        F: FnOnce(&mut DaemonStatus),
    {
        let mut status = self.status_tx.borrow().clone();
        update(&mut status);
        status.updated_at_ms = now_ms();
        self.status_tx.send_replace(status.clone());
        self.emit(
            "status",
            serde_json::to_value(status).unwrap_or(Value::Null),
        );
    }

    fn emit(&self, kind: &str, payload: Value) {
        let _ = self.events.send(ServerEvent::new(kind, payload));
    }

    async fn push_log(&self, stream: &str, line: String) {
        let entry = LogEntry {
            timestamp_ms: now_ms(),
            stream: stream.into(),
            line,
        };
        let mut logs = self.logs.lock().await;
        if logs.len() == LOG_CAPACITY {
            logs.pop_front();
        }
        logs.push_back(entry.clone());
        drop(logs);
        self.emit("log", serde_json::to_value(entry).unwrap_or(Value::Null));
    }

    async fn persist(&self) -> anyhow::Result<()> {
        let state = self.persistent.read().await.clone();
        self.store.save(&state).await
    }
}

async fn write_worker_config(path: &Path, config: &WorkerConfig) -> anyhow::Result<()> {
    let data = serde_json::to_vec_pretty(config)?;
    tokio::fs::write(path, data).await?;
    Ok(())
}

fn spawn_stdout_reader(
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

fn spawn_stderr_reader(
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

fn spawn_waiter(
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

fn signal_process(pid: u32, signal: Signal) -> Result<(), SupervisorError> {
    kill(Pid::from_raw(pid as i32), signal)
        .map_err(|error| SupervisorError::Operation(error.to_string()))
}

fn new_generation() -> String {
    Uuid::new_v4().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::PersistentState;
    use std::os::unix::fs::PermissionsExt;
    use tempfile::tempdir;

    #[test]
    fn process_state_transition_detection() {
        assert!(ProcessState::Starting.is_transitioning());
        assert!(ProcessState::RollingBack.is_transitioning());
        assert!(!ProcessState::Running.is_transitioning());
    }

    #[test]
    fn restart_backoff_is_bounded() {
        let delays = [1_u64, 2, 4, 8, 16, 30];
        assert_eq!(delays[0], 1);
        assert_eq!(*delays.last().unwrap(), 30);
    }

    #[tokio::test]
    async fn fake_worker_reaches_ready_captures_logs_and_stops() {
        let temp = tempdir().unwrap();
        let worker = temp.path().join("fake-worker.sh");
        std::fs::write(
            &worker,
            r#"#!/bin/sh
config_path=""
previous=""
for arg in "$@"; do
  if [ "$previous" = "--config" ]; then config_path="$arg"; fi
  if [ "$arg" = "--validate-only" ]; then
    printf '%s\n' '{"event":"Stopped","reason":"validation_only"}'
    exit 0
  fi
  previous="$arg"
done
if grep -q 'fail-start' "$config_path"; then
  printf '%s\n' '{"event":"FatalError","message":"injected startup failure"}'
  exit 12
fi
printf '%s\n' 'fake worker started' >&2
printf '%s\n' '{"event":"StreamReady","media":"video"}'
printf '%s\n' '{"event":"StreamReady","media":"audio"}'
printf '%s\n' '{"event":"Metrics","video":{"packets":3},"audio":{"packets":2}}'
printf '\101\111\120\126\000\001\000\001\000\000\000\030\000\000\000\000\000\000\000\052\000\000\000\000\000\000\000\001\000\000\000\001\147\144\000\050\000\000\000\001\150\001\002\003\000\000\000\001\145\011\010\007' >&3 || true
trap 'exit 0' TERM INT
while :; do sleep 1; done
"#,
        )
        .unwrap();
        let mut permissions = std::fs::metadata(&worker).unwrap().permissions();
        permissions.set_mode(0o755);
        std::fs::set_permissions(&worker, permissions).unwrap();
        let settings = DaemonConfig {
            worker_path: worker,
            data_dir: temp.path().join("data"),
            runtime_dir: temp.path().join("run"),
            startup_timeout_ms: 2_000,
            stop_timeout_ms: 500,
            autostart: true,
            ..DaemonConfig::default()
        };
        let handle =
            spawn_supervisor(settings, PersistentState::new(WorkerConfig::default())).await;
        wait_for_state(&handle, ProcessState::Running).await;
        assert!(handle.status.borrow().video_ready);
        assert!(handle.status.borrow().audio_ready);
        wait_for_preview(&handle).await;
        assert!(handle.preview.status().available);
        assert_eq!(
            handle.status.borrow().metrics.as_ref().unwrap()["video"]["packets"],
            3
        );
        tokio::time::sleep(Duration::from_millis(50)).await;
        assert!(
            handle
                .logs
                .lock()
                .await
                .iter()
                .any(|entry| entry.line.contains("fake worker started"))
        );

        let mut updated = WorkerConfig::default();
        updated.video.width = 1280;
        updated.video.height = 720;
        let accepted = handle.apply(updated.clone()).await.unwrap();
        let applied_generation = accepted.generation.unwrap();
        wait_for_generation(&handle, &applied_generation).await;
        assert_eq!(handle.status.borrow().restart_count, 1);
        wait_for_preview(&handle).await;
        assert_eq!(
            handle.preview.status().generation.as_deref(),
            Some(applied_generation.as_str())
        );
        assert_eq!(
            handle
                .persistent
                .read()
                .await
                .last_good
                .as_ref()
                .unwrap()
                .video
                .width,
            1280
        );

        let mut failing = updated;
        failing.video.output_path = "/tmp/fail-start.h264".into();
        let failed_generation = handle.apply(failing).await.unwrap().generation.unwrap();
        wait_for_rollback(&handle, &failed_generation).await;
        wait_for_preview(&handle).await;
        let persistent = handle.persistent.read().await;
        assert!(persistent.last_error.is_some());
        assert_eq!(persistent.active.as_ref().unwrap().video.width, 1280);
        drop(persistent);
        assert_eq!(handle.status.borrow().restart_count, 3);

        handle.stop().await.unwrap();
        wait_for_state(&handle, ProcessState::Stopped).await;
        handle.shutdown().await;
    }

    async fn wait_for_state(handle: &SupervisorHandle, expected: ProcessState) {
        let mut status = handle.status.clone();
        tokio::time::timeout(Duration::from_secs(4), async {
            loop {
                if status.borrow().state == expected {
                    return;
                }
                status.changed().await.unwrap();
            }
        })
        .await
        .unwrap();
    }

    async fn wait_for_generation(handle: &SupervisorHandle, generation: &str) {
        let mut status = handle.status.clone();
        tokio::time::timeout(Duration::from_secs(4), async {
            loop {
                if status.borrow().state == ProcessState::Running
                    && status.borrow().generation.as_deref() == Some(generation)
                {
                    return;
                }
                status.changed().await.unwrap();
            }
        })
        .await
        .unwrap();
    }

    async fn wait_for_rollback(handle: &SupervisorHandle, failed_generation: &str) {
        let mut status = handle.status.clone();
        tokio::time::timeout(Duration::from_secs(4), async {
            loop {
                if status.borrow().state == ProcessState::Running
                    && status.borrow().generation.as_deref() != Some(failed_generation)
                    && status.borrow().last_error.is_some()
                {
                    return;
                }
                status.changed().await.unwrap();
            }
        })
        .await
        .unwrap();
    }

    async fn wait_for_preview(handle: &SupervisorHandle) {
        tokio::time::timeout(Duration::from_secs(2), async {
            while !handle.preview.status().available {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .unwrap();
    }
}
