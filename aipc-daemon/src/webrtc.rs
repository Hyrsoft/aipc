use crate::config::WebRtcConfig;
use crate::model::ServerEvent;
use crate::preview::{AudioFrame, PreviewFrame, VideoHub};
use bytes::BytesMut;
use serde::Serialize;
use serde_json::json;
use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};
use str0m::change::SdpOffer;
use str0m::format::Codec;
use str0m::media::{Frequency, MediaKind, MediaTime, Mid};
use str0m::net::{Protocol, Receive};
use str0m::{Candidate, Event, IceConnectionState, Input, Output, Rtc, RtcConfig};
use thiserror::Error;
use tokio::net::UdpSocket;
use tokio::sync::{broadcast, mpsc, oneshot};
use uuid::Uuid;

const STR0M_RTP_OVERHEAD_BUDGET: usize = 80;

#[derive(Debug, Error)]
pub enum WebRtcError {
    #[error("WebRTC is disabled")]
    Disabled,
    #[error("WebRTC media is not ready")]
    NotReady,
    #[error("WebRTC client limit reached")]
    ClientLimit,
    #[error("invalid WebRTC offer: {0}")]
    InvalidOffer(String),
    #[error("WebRTC codec negotiation failed: {0}")]
    Codec(String),
    #[error("WebRTC session not found")]
    NotFound,
    #[error("WebRTC operation failed: {0}")]
    Operation(String),
}

#[derive(Debug, Clone, Serialize)]
pub struct WebRtcStatus {
    pub enabled: bool,
    pub listening: bool,
    pub bind: String,
    pub advertised_ip: Option<IpAddr>,
    pub clients: usize,
    pub max_clients: usize,
    pub generation: Option<String>,
    pub video_available: bool,
    pub audio_available: bool,
    pub video_codec: &'static str,
    pub video_profile_level_id: Option<String>,
    pub video_sps_profile_level_id: Option<String>,
    pub audio_codec: &'static str,
    pub video_frames: u64,
    pub audio_packets: u64,
    pub dropped_frames: u64,
    pub errors: u64,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct SessionAnswer {
    pub id: String,
    pub r#type: &'static str,
    pub sdp: String,
}

#[derive(Clone)]
pub struct WebRtcServer {
    config: WebRtcConfig,
    commands: Option<mpsc::Sender<Command>>,
    status: Arc<RwLock<WebRtcStatus>>,
}

impl WebRtcServer {
    pub async fn start(
        config: WebRtcConfig,
        hub: VideoHub,
        events: broadcast::Sender<ServerEvent>,
    ) -> anyhow::Result<Self> {
        let status = Arc::new(RwLock::new(WebRtcStatus {
            enabled: config.enabled,
            listening: false,
            bind: config.bind.clone(),
            advertised_ip: config.advertised_ip,
            clients: 0,
            max_clients: config.max_clients,
            generation: None,
            video_available: false,
            audio_available: false,
            video_codec: "h264",
            video_profile_level_id: None,
            video_sps_profile_level_id: None,
            audio_codec: "pcma",
            video_frames: 0,
            audio_packets: 0,
            dropped_frames: 0,
            errors: 0,
            last_error: None,
        }));
        if !config.enabled {
            return Ok(Self {
                config,
                commands: None,
                status,
            });
        }

        str0m::crypto::from_feature_flags().install_process_default();
        let socket = UdpSocket::bind(&config.bind).await?;
        let (command_tx, command_rx) = mpsc::channel(32);
        let actor = WebRtcActor {
            config: config.clone(),
            socket,
            hub: hub.clone(),
            video_rx: hub.subscribe(),
            audio_rx: hub.subscribe_audio(),
            command_rx,
            sessions: HashMap::new(),
            status: status.clone(),
            events,
        };
        status.write().unwrap().listening = true;
        tokio::spawn(actor.run());
        Ok(Self {
            config,
            commands: Some(command_tx),
            status,
        })
    }

    pub fn status(&self) -> WebRtcStatus {
        self.status.read().unwrap().clone()
    }

    pub async fn create_session(
        &self,
        offer_sdp: String,
        remote: SocketAddr,
    ) -> Result<SessionAnswer, WebRtcError> {
        let Some(commands) = &self.commands else {
            return Err(WebRtcError::Disabled);
        };
        let advertised_ip = match self.config.advertised_ip {
            Some(value) => value,
            None => route_local_ip(remote).await?,
        };
        let (reply, receiver) = oneshot::channel();
        commands
            .send(Command::Create {
                offer_sdp,
                advertised_ip,
                reply,
            })
            .await
            .map_err(|_| WebRtcError::Operation("server task stopped".into()))?;
        receiver
            .await
            .map_err(|_| WebRtcError::Operation("server reply dropped".into()))?
    }

    pub async fn delete_session(&self, id: &str) -> Result<(), WebRtcError> {
        let Some(commands) = &self.commands else {
            return Err(WebRtcError::Disabled);
        };
        let (reply, receiver) = oneshot::channel();
        commands
            .send(Command::Delete {
                id: id.to_owned(),
                reply,
            })
            .await
            .map_err(|_| WebRtcError::Operation("server task stopped".into()))?;
        receiver
            .await
            .map_err(|_| WebRtcError::Operation("server reply dropped".into()))?
    }

    pub async fn shutdown(&self) {
        let Some(commands) = &self.commands else {
            return;
        };
        let (reply, receiver) = oneshot::channel();
        let _ = commands.send(Command::Shutdown { reply }).await;
        let _ = receiver.await;
    }
}

async fn route_local_ip(remote: SocketAddr) -> Result<IpAddr, WebRtcError> {
    let bind = if remote.is_ipv4() {
        "0.0.0.0:0"
    } else {
        "[::]:0"
    };
    let socket = UdpSocket::bind(bind)
        .await
        .map_err(|error| WebRtcError::Operation(error.to_string()))?;
    socket
        .connect(remote)
        .await
        .map_err(|error| WebRtcError::Operation(error.to_string()))?;
    Ok(socket
        .local_addr()
        .map_err(|error| WebRtcError::Operation(error.to_string()))?
        .ip())
}

enum Command {
    Create {
        offer_sdp: String,
        advertised_ip: IpAddr,
        reply: oneshot::Sender<Result<SessionAnswer, WebRtcError>>,
    },
    Delete {
        id: String,
        reply: oneshot::Sender<Result<(), WebRtcError>>,
    },
    Shutdown {
        reply: oneshot::Sender<()>,
    },
}

struct Peer {
    rtc: Rtc,
    candidate_addr: SocketAddr,
    generation: String,
    video_mid: Option<Mid>,
    audio_mid: Option<Mid>,
    connected: bool,
    waiting_keyframe: bool,
    base_pts: Option<u64>,
    wallclock: Option<Instant>,
    created_at: Instant,
    last_activity: Instant,
    timeout: Instant,
    alive: bool,
}

struct WebRtcActor {
    config: WebRtcConfig,
    socket: UdpSocket,
    hub: VideoHub,
    video_rx: broadcast::Receiver<Arc<PreviewFrame>>,
    audio_rx: broadcast::Receiver<Arc<AudioFrame>>,
    command_rx: mpsc::Receiver<Command>,
    sessions: HashMap<String, Peer>,
    status: Arc<RwLock<WebRtcStatus>>,
    events: broadcast::Sender<ServerEvent>,
}

impl WebRtcActor {
    async fn run(mut self) {
        let mut packet = vec![0_u8; 2048];
        loop {
            let deadline = self
                .sessions
                .values()
                .map(|peer| peer.timeout)
                .min()
                .unwrap_or_else(|| Instant::now() + Duration::from_secs(3600));
            tokio::select! {
                command = self.command_rx.recv() => match command {
                    Some(Command::Create { offer_sdp, advertised_ip, reply }) => {
                        let result = self.create_peer(offer_sdp, advertised_ip).await;
                        let _ = reply.send(result);
                    }
                    Some(Command::Delete { id, reply }) => {
                        let result = self.sessions.remove(&id).map(|_| ()).ok_or(WebRtcError::NotFound);
                        self.update_status();
                        let _ = reply.send(result);
                    }
                    Some(Command::Shutdown { reply }) => {
                        self.sessions.clear();
                        self.status.write().unwrap().listening = false;
                        let _ = reply.send(());
                        break;
                    }
                    None => break,
                },
                result = self.socket.recv_from(&mut packet) => {
                    match result {
                        Ok((size, source)) => self.handle_datagram(&packet[..size], source).await,
                        Err(error) => self.record_error(error.to_string()),
                    }
                }
                result = self.video_rx.recv() => self.handle_video(result).await,
                result = self.audio_rx.recv() => self.handle_audio(result).await,
                _ = tokio::time::sleep_until(deadline.into()) => self.handle_timeouts().await,
            }
            self.remove_dead();
            self.update_status();
        }
    }

    async fn create_peer(
        &mut self,
        offer_sdp: String,
        advertised_ip: IpAddr,
    ) -> Result<SessionAnswer, WebRtcError> {
        if self.sessions.len() >= self.config.max_clients {
            return Err(WebRtcError::ClientLimit);
        }
        let preview = self.hub.status();
        let codec = self.hub.codec_config().ok_or(WebRtcError::NotReady)?;
        if !preview.available || codec.sps.len() < 4 {
            return Err(WebRtcError::NotReady);
        }
        let raw_profile_level_id = h264_sps_profile_level_id(&codec.sps)
            .ok_or_else(|| WebRtcError::Codec("SPS does not contain profile-level-id".into()))?;
        let profile_level_id = normalize_h264_profile_level_id(raw_profile_level_id);
        let offered_profiles = offered_h264_profiles(&offer_sdp);
        let offer = SdpOffer::from_sdp_string(&offer_sdp)
            .map_err(|error| WebRtcError::InvalidOffer(error.to_string()))?;
        let now = Instant::now();
        let mut builder = RtcConfig::default()
            .set_ice_lite(true)
            .set_mtu((self.config.mtu - STR0M_RTP_OVERHEAD_BUDGET)..=self.config.mtu)
            .clear_codecs()
            .enable_pcma(preview.audio.available);
        builder
            .codec_config()
            .add_h264(114_u8.into(), Some(115_u8.into()), true, profile_level_id);
        let mut rtc = builder.build(now);
        let port = self
            .socket
            .local_addr()
            .map_err(|error| WebRtcError::Operation(error.to_string()))?
            .port();
        let candidate_addr = SocketAddr::new(advertised_ip, port);
        let _ = rtc.add_local_candidate(
            Candidate::host(candidate_addr, "udp")
                .map_err(|error| WebRtcError::Operation(error.to_string()))?,
        );
        let answer = rtc
            .sdp_api()
            .accept_offer(offer)
            .map_err(|error| WebRtcError::Codec(error.to_string()))?;
        let answer_sdp = answer.to_sdp_string();
        let (video_mid, audio_mid) = negotiated_send_mids(&answer_sdp);
        let id = Uuid::new_v4().to_string();
        let mut peer = Peer {
            rtc,
            candidate_addr,
            generation: codec.info.generation.clone(),
            video_mid,
            audio_mid,
            connected: false,
            waiting_keyframe: true,
            base_pts: None,
            wallclock: None,
            created_at: now,
            last_activity: now,
            timeout: now,
            alive: true,
        };
        drain_peer(&self.socket, &mut peer).await?;
        if peer.video_mid.is_none() {
            return Err(WebRtcError::Codec(format!(
                "offer did not negotiate send-only H264; active profile-level-id={profile_level_id:06x} (SPS={raw_profile_level_id:06x}), offered={offered_profiles:?}"
            )));
        }
        self.sessions.insert(id.clone(), peer);
        self.status.write().unwrap().advertised_ip = Some(advertised_ip);
        let _ = self.events.send(ServerEvent::new(
            "webrtc",
            json!({"action":"session_created","id":id,"generation":codec.info.generation}),
        ));
        Ok(SessionAnswer {
            id,
            r#type: "answer",
            sdp: answer_sdp,
        })
    }

    async fn handle_datagram(&mut self, data: &[u8], source: SocketAddr) {
        let now = Instant::now();
        let mut failure = None;
        for peer in self.sessions.values_mut() {
            let Ok(contents) = data.try_into() else {
                failure = Some("invalid WebRTC datagram".into());
                break;
            };
            let input = Input::Receive(
                now,
                Receive {
                    proto: Protocol::Udp,
                    source,
                    destination: peer.candidate_addr,
                    contents,
                },
            );
            if !peer.rtc.accepts(&input) {
                continue;
            }
            peer.last_activity = now;
            if let Err(error) = peer.rtc.handle_input(input) {
                peer.alive = false;
                failure = Some(error.to_string());
            } else if let Err(error) = drain_peer(&self.socket, peer).await {
                peer.alive = false;
                failure = Some(error.to_string());
            }
            break;
        }
        if let Some(error) = failure {
            self.record_error(error);
        }
    }

    async fn handle_video(
        &mut self,
        result: Result<Arc<PreviewFrame>, broadcast::error::RecvError>,
    ) {
        let frame = match result {
            Ok(frame) => frame,
            Err(broadcast::error::RecvError::Lagged(skipped)) => {
                for peer in self.sessions.values_mut() {
                    peer.waiting_keyframe = true;
                }
                self.status.write().unwrap().dropped_frames += skipped;
                return;
            }
            Err(broadcast::error::RecvError::Closed) => return,
        };
        let codec = self.hub.codec_config();
        let mut failures = Vec::new();
        for (id, peer) in &mut self.sessions {
            if peer.generation != frame.info.generation {
                peer.alive = false;
                continue;
            }
            if !peer.connected || peer.video_mid.is_none() {
                continue;
            }
            if peer.waiting_keyframe && !frame.keyframe {
                continue;
            }
            let mut data = None;
            if peer.waiting_keyframe {
                let Some(codec) = codec.as_ref() else {
                    continue;
                };
                let mut prefixed = BytesMut::with_capacity(
                    codec.sps.len() + codec.pps.len() + frame.data.len() + 8,
                );
                prefixed.extend_from_slice(&[0, 0, 0, 1]);
                prefixed.extend_from_slice(&codec.sps);
                prefixed.extend_from_slice(&[0, 0, 0, 1]);
                prefixed.extend_from_slice(&codec.pps);
                prefixed.extend_from_slice(&frame.data);
                data = Some(prefixed.freeze());
                peer.waiting_keyframe = false;
                peer.base_pts = Some(frame.pts);
                peer.wallclock = Some(Instant::now());
            }
            let Some(base_pts) = peer.base_pts else {
                continue;
            };
            let delta = frame.pts.saturating_sub(base_pts);
            let wallclock = peer.wallclock.unwrap() + Duration::from_micros(delta);
            let payload = data.as_ref().unwrap_or(&frame.data);
            if let Err(error) = write_media(
                &mut peer.rtc,
                peer.video_mid.unwrap(),
                Codec::H264,
                wallclock,
                MediaTime::from_90khz(delta.saturating_mul(90) / 1000),
                payload,
            ) {
                failures.push((id.clone(), error));
                continue;
            }
            if let Err(error) = drain_peer(&self.socket, peer).await {
                failures.push((id.clone(), error));
            } else {
                self.status.write().unwrap().video_frames += 1;
            }
        }
        for (id, error) in failures {
            if let Some(peer) = self.sessions.get_mut(&id) {
                peer.alive = false;
            }
            self.record_error(error.to_string());
        }
    }

    async fn handle_audio(&mut self, result: Result<Arc<AudioFrame>, broadcast::error::RecvError>) {
        let frame = match result {
            Ok(frame) => frame,
            Err(broadcast::error::RecvError::Lagged(skipped)) => {
                self.status.write().unwrap().dropped_frames += skipped;
                return;
            }
            Err(broadcast::error::RecvError::Closed) => return,
        };
        let mut failures = Vec::new();
        for (id, peer) in &mut self.sessions {
            let (Some(mid), Some(base_pts), Some(anchor)) =
                (peer.audio_mid, peer.base_pts, peer.wallclock)
            else {
                continue;
            };
            if !peer.connected || peer.generation != frame.info.generation || frame.pts < base_pts {
                continue;
            }
            let delta = frame.pts - base_pts;
            if let Err(error) = write_media(
                &mut peer.rtc,
                mid,
                Codec::PCMA,
                anchor + Duration::from_micros(delta),
                MediaTime::new(delta.saturating_mul(8) / 1000, Frequency::EIGHT_KHZ),
                &frame.data,
            ) {
                failures.push((id.clone(), error));
                continue;
            }
            if let Err(error) = drain_peer(&self.socket, peer).await {
                failures.push((id.clone(), error));
            } else {
                self.status.write().unwrap().audio_packets += 1;
            }
        }
        for (id, error) in failures {
            if let Some(peer) = self.sessions.get_mut(&id) {
                peer.alive = false;
            }
            self.record_error(error.to_string());
        }
    }

    async fn handle_timeouts(&mut self) {
        let now = Instant::now();
        let connect_timeout = Duration::from_millis(self.config.connect_timeout_ms);
        let idle_timeout = Duration::from_millis(self.config.idle_timeout_ms);
        let current_generation = self.hub.status().generation;
        let mut errors = Vec::new();
        for peer in self.sessions.values_mut() {
            if current_generation.as_deref() != Some(&peer.generation)
                || (!peer.connected && now.duration_since(peer.created_at) >= connect_timeout)
                || (peer.connected && now.duration_since(peer.last_activity) >= idle_timeout)
            {
                peer.alive = false;
                continue;
            }
            if peer.timeout <= now {
                if let Err(error) = peer.rtc.handle_input(Input::Timeout(now)) {
                    peer.alive = false;
                    errors.push(error.to_string());
                } else if let Err(error) = drain_peer(&self.socket, peer).await {
                    peer.alive = false;
                    errors.push(error.to_string());
                }
            }
        }
        for error in errors {
            self.record_error(error);
        }
    }

    fn remove_dead(&mut self) {
        self.sessions
            .retain(|_, peer| peer.alive && peer.rtc.is_alive());
    }

    fn update_status(&self) {
        let preview = self.hub.status();
        let mut status = self.status.write().unwrap();
        status.clients = self.sessions.len();
        status.generation = preview.generation;
        status.video_available = preview.available;
        status.audio_available = preview.audio.available;
        let profile_ids = self.hub.codec_config().and_then(|codec| {
            let raw = h264_sps_profile_level_id(&codec.sps)?;
            Some((normalize_h264_profile_level_id(raw), raw))
        });
        status.video_profile_level_id = profile_ids.map(|(value, _)| format!("{value:06x}"));
        status.video_sps_profile_level_id = profile_ids.map(|(_, value)| format!("{value:06x}"));
    }

    fn record_error(&self, message: String) {
        let mut status = self.status.write().unwrap();
        status.errors += 1;
        status.last_error = Some(message.clone());
        drop(status);
        let _ = self.events.send(ServerEvent::new(
            "webrtc",
            json!({"action":"error","message":message}),
        ));
    }
}

fn negotiated_send_mids(sdp: &str) -> (Option<Mid>, Option<Mid>) {
    #[derive(Default)]
    struct Section {
        kind: Option<MediaKind>,
        enabled: bool,
        sending: bool,
        mid: Option<Mid>,
        h264: bool,
        pcma: bool,
    }

    fn commit(section: &Section, video: &mut Option<Mid>, audio: &mut Option<Mid>) {
        if !section.enabled || !section.sending {
            return;
        }
        match section.kind {
            Some(MediaKind::Video) if section.h264 => *video = section.mid,
            Some(MediaKind::Audio) if section.pcma => *audio = section.mid,
            _ => {}
        }
    }

    let mut section = Section::default();
    let mut video = None;
    let mut audio = None;
    for line in sdp.lines().map(str::trim) {
        if let Some(media) = line.strip_prefix("m=") {
            commit(&section, &mut video, &mut audio);
            section = Section::default();
            let mut fields = media.split_whitespace();
            section.kind = match fields.next() {
                Some("video") => Some(MediaKind::Video),
                Some("audio") => Some(MediaKind::Audio),
                _ => None,
            };
            section.enabled = fields.next().is_some_and(|port| port != "0");
        } else if let Some(mid) = line.strip_prefix("a=mid:") {
            section.mid = Some(Mid::from(mid));
        } else if line == "a=sendonly" || line == "a=sendrecv" {
            section.sending = true;
        } else if line.to_ascii_uppercase().contains(" H264/90000") {
            section.h264 = true;
        } else if line.to_ascii_uppercase().contains(" PCMA/8000") {
            section.pcma = true;
        }
    }
    commit(&section, &mut video, &mut audio);
    (video, audio)
}

fn h264_sps_profile_level_id(sps: &[u8]) -> Option<u32> {
    (sps.len() >= 4 && sps[0] & 0x1f == 7)
        .then(|| ((sps[1] as u32) << 16) | ((sps[2] as u32) << 8) | sps[3] as u32)
}

fn normalize_h264_profile_level_id(value: u32) -> u32 {
    let profile_idc = (value >> 16) & 0xff;
    if profile_idc == 0x64 {
        value & 0xff00ff
    } else {
        value
    }
}

fn offered_h264_profiles(sdp: &str) -> Vec<String> {
    let mut profiles = sdp
        .lines()
        .filter_map(|line| {
            line.split(';').find_map(|parameter| {
                parameter
                    .trim()
                    .strip_prefix("profile-level-id=")
                    .map(str::to_ascii_lowercase)
            })
        })
        .collect::<Vec<_>>();
    profiles.sort();
    profiles.dedup();
    profiles
}

fn write_media(
    rtc: &mut Rtc,
    mid: Mid,
    codec: Codec,
    wallclock: Instant,
    media_time: MediaTime,
    data: &[u8],
) -> Result<(), WebRtcError> {
    let writer = rtc
        .writer(mid)
        .ok_or_else(|| WebRtcError::Codec("negotiated media is not sendable".into()))?;
    let pt = writer
        .payload_params()
        .find(|params| params.spec().codec == codec)
        .map(|params| params.pt())
        .ok_or_else(|| WebRtcError::Codec(format!("{codec} was not negotiated")))?;
    writer
        .write(pt, wallclock, media_time, Arc::<[u8]>::from(data))
        .map_err(|error| WebRtcError::Operation(error.to_string()))
}

async fn drain_peer(socket: &UdpSocket, peer: &mut Peer) -> Result<(), WebRtcError> {
    loop {
        match peer
            .rtc
            .poll_output()
            .map_err(|error| WebRtcError::Operation(error.to_string()))?
        {
            Output::Timeout(timeout) => {
                peer.timeout = timeout;
                return Ok(());
            }
            Output::Transmit(transmit) => socket
                .send_to(&transmit.contents, transmit.destination)
                .await
                .map(|_| ())
                .map_err(|error| WebRtcError::Operation(error.to_string()))?,
            Output::Event(event) => match event {
                Event::IceConnectionStateChange(state) => match state {
                    IceConnectionState::Connected | IceConnectionState::Completed => {
                        peer.connected = true;
                        peer.last_activity = Instant::now();
                    }
                    IceConnectionState::Disconnected => peer.alive = false,
                    _ => {}
                },
                Event::MediaAdded(media) if media.direction.is_sending() => match media.kind {
                    MediaKind::Video => peer.video_mid = Some(media.mid),
                    MediaKind::Audio => peer.audio_mid = Some(media.mid),
                },
                Event::KeyframeRequest(_) => peer.waiting_keyframe = true,
                _ => {}
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::PreviewConfig;
    use crate::preview::{AudioStreamInfo, StreamInfo};
    use bytes::Bytes;
    use str0m::change::SdpAnswer;
    use str0m::media::Direction;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    #[test]
    fn media_time_conversion_is_stable() {
        let delta = 1_000_000_u64;
        assert_eq!(MediaTime::from_90khz(delta * 90 / 1000).numer(), 90_000);
        assert_eq!(
            MediaTime::new(delta * 8 / 1000, Frequency::EIGHT_KHZ).numer(),
            8_000
        );
    }

    #[test]
    fn normalizes_high_profile_sps_constraints_for_sdp() {
        assert_eq!(
            h264_sps_profile_level_id(&[0x67, 0x64, 0x10, 0x28]),
            Some(0x641028)
        );
        assert_eq!(normalize_h264_profile_level_id(0x641028), 0x640028);
        assert_eq!(normalize_h264_profile_level_id(0x42e01f), 0x42e01f);
    }

    #[test]
    fn extracts_only_enabled_send_mids() {
        let sdp = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 114\r\na=mid:0\r\na=sendonly\r\na=rtpmap:114 H264/90000\r\nm=audio 9 UDP/TLS/RTP/SAVPF 8\r\na=mid:1\r\na=sendonly\r\na=rtpmap:8 PCMA/8000\r\n";
        let (video, audio) = negotiated_send_mids(sdp);
        assert_eq!(video.unwrap().to_string(), "0");
        assert_eq!(audio.unwrap().to_string(), "1");
    }

    #[tokio::test]
    async fn negotiates_h264_session_from_ready_media_bus() {
        str0m::crypto::from_feature_flags().install_process_default();
        let (events, _) = broadcast::channel(16);
        let hub = VideoHub::new(PreviewConfig::default(), events.clone());
        let info = StreamInfo {
            generation: "test-generation".into(),
            codec: "h264",
            format: "annexb",
            width: 1920,
            height: 1080,
            fps: 30,
        };
        hub.begin_generation(info.clone());
        hub.ingest(PreviewFrame {
            info,
            pts: 1_000_000,
            sequence: 1,
            keyframe: true,
            data: Bytes::from_static(&[
                0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f, 0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80, 0, 0, 0, 1,
                0x65, 0x88, 0x84,
            ]),
        });
        hub.begin_audio_generation(AudioStreamInfo {
            generation: "test-generation".into(),
            codec: "g711a",
            sample_rate: 8_000,
            channels: 1,
            bit_width: 16,
            bitrate: 64_000,
        });
        hub.ingest_audio(AudioFrame {
            info: AudioStreamInfo {
                generation: "test-generation".into(),
                codec: "g711a",
                sample_rate: 8_000,
                channels: 1,
                bit_width: 16,
                bitrate: 64_000,
            },
            pts: 1_000_000,
            sequence: 1,
            data: Bytes::from_static(&[0xd5; 160]),
        });

        let server = WebRtcServer::start(
            WebRtcConfig {
                bind: "127.0.0.1:0".into(),
                advertised_ip: Some("127.0.0.1".parse().unwrap()),
                ..WebRtcConfig::default()
            },
            hub,
            events,
        )
        .await
        .unwrap();

        let now = Instant::now();
        let mut config = RtcConfig::default().clear_codecs().set_ice_lite(false);
        config
            .codec_config()
            .add_h264(114_u8.into(), Some(115_u8.into()), true, 0x64001f);
        config.codec_config().enable_pcma(true);
        let mut browser = config.build(now);
        let _ = browser.add_local_candidate(
            Candidate::host("127.0.0.1:50000".parse().unwrap(), "udp").unwrap(),
        );
        let mut changes = browser.sdp_api();
        changes.add_media(MediaKind::Video, Direction::RecvOnly, None, None, None);
        changes.add_media(MediaKind::Audio, Direction::RecvOnly, None, None, None);
        let (offer, _) = changes.apply().unwrap();

        let answer = server
            .create_session(offer.to_sdp_string(), "127.0.0.1:40000".parse().unwrap())
            .await
            .unwrap();
        assert_eq!(answer.r#type, "answer");
        assert!(answer.sdp.contains("H264/90000"));
        assert!(answer.sdp.contains("PCMA/8000"));
        assert_eq!(server.status().clients, 1);
        server.delete_session(&answer.id).await.unwrap();
        assert_eq!(server.status().clients, 0);
        server.shutdown().await;
    }

    #[tokio::test]
    #[ignore = "requires a live AIPC board; set AIPC_WEBRTC_BOARD"]
    async fn live_board_transports_h264_and_pcma() {
        str0m::crypto::from_feature_flags().install_process_default();
        let board = std::env::var("AIPC_WEBRTC_BOARD")
            .expect("set AIPC_WEBRTC_BOARD, for example 192.168.8.106:8080");
        let http_addr: SocketAddr = board.parse().expect("AIPC_WEBRTC_BOARD socket address");
        let profile = std::env::var("AIPC_WEBRTC_PROFILE_LEVEL_ID")
            .ok()
            .map(|value| u32::from_str_radix(&value, 16).expect("hex profile-level-id"))
            .unwrap_or(0x640028);

        let route = std::net::UdpSocket::bind("0.0.0.0:0").unwrap();
        route
            .connect(SocketAddr::new(http_addr.ip(), 10_000))
            .unwrap();
        let local_ip = route.local_addr().unwrap().ip();
        drop(route);
        let socket = UdpSocket::bind(SocketAddr::new(local_ip, 0)).await.unwrap();
        let candidate_addr = socket.local_addr().unwrap();

        let now = Instant::now();
        let mut config = RtcConfig::default().clear_codecs().set_ice_lite(false);
        config
            .codec_config()
            .add_h264(114_u8.into(), Some(115_u8.into()), true, profile);
        config.codec_config().enable_pcma(true);
        let mut peer = config.build(now);
        peer.add_local_candidate(Candidate::host(candidate_addr, "udp").unwrap());
        let mut changes = peer.sdp_api();
        changes.add_media(MediaKind::Video, Direction::RecvOnly, None, None, None);
        changes.add_media(MediaKind::Audio, Direction::RecvOnly, None, None, None);
        let (offer, pending) = changes.apply().unwrap();

        let body =
            serde_json::to_vec(&json!({"type":"offer","sdp":offer.to_sdp_string()})).unwrap();
        let (status, response) = board_http(&board, "POST", "/api/v1/webrtc/sessions", &body).await;
        assert_eq!(
            status,
            201,
            "signaling response: {}",
            String::from_utf8_lossy(&response)
        );
        let answer: serde_json::Value = serde_json::from_slice(&response).unwrap();
        let session_id = answer["id"].as_str().unwrap().to_owned();
        let answer = SdpAnswer::from_sdp_string(answer["sdp"].as_str().unwrap()).unwrap();
        peer.sdp_api().accept_answer(pending, answer).unwrap();

        let mut packet = vec![0_u8; 2048];
        let deadline = Instant::now() + Duration::from_secs(20);
        let mut connected = false;
        let mut video_frames = 0_u64;
        let mut audio_packets = 0_u64;
        let mut max_datagram_size = 0_usize;
        while Instant::now() < deadline && (video_frames == 0 || audio_packets == 0) {
            let timeout = loop {
                match peer.poll_output().unwrap() {
                    Output::Timeout(value) => break value,
                    Output::Transmit(transmit) => {
                        socket
                            .send_to(&transmit.contents, transmit.destination)
                            .await
                            .unwrap();
                    }
                    Output::Event(Event::Connected) => connected = true,
                    Output::Event(Event::MediaData(data)) => match data.params.spec().codec {
                        Codec::H264 => video_frames += 1,
                        Codec::PCMA => audio_packets += 1,
                        _ => {}
                    },
                    Output::Event(_) => {}
                }
            };

            tokio::select! {
                received = socket.recv_from(&mut packet) => {
                    let (size, source) = received.unwrap();
                    max_datagram_size = max_datagram_size.max(size);
                    let contents = (&packet[..size]).try_into().unwrap();
                    peer.handle_input(Input::Receive(
                        Instant::now(),
                        Receive {
                            proto: Protocol::Udp,
                            source,
                            destination: candidate_addr,
                            contents,
                        },
                    )).unwrap();
                }
                _ = tokio::time::sleep_until(timeout.into()) => {
                    peer.handle_input(Input::Timeout(Instant::now())).unwrap();
                }
            }
        }

        let (delete_status, _) = board_http(
            &board,
            "DELETE",
            &format!("/api/v1/webrtc/sessions/{session_id}"),
            &[],
        )
        .await;
        assert_eq!(delete_status, 204);
        assert!(connected, "ICE/DTLS did not connect");
        assert!(video_frames > 0, "no H264 media received");
        assert!(audio_packets > 0, "no PCMA media received");
        assert!(
            max_datagram_size <= 1200,
            "received UDP datagram exceeded configured MTU: {max_datagram_size}"
        );
    }

    async fn board_http(board: &str, method: &str, path: &str, body: &[u8]) -> (u16, Vec<u8>) {
        let mut stream = tokio::net::TcpStream::connect(board).await.unwrap();
        let request = format!(
            "{method} {path} HTTP/1.1\r\nHost: {board}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
            body.len()
        );
        stream.write_all(request.as_bytes()).await.unwrap();
        stream.write_all(body).await.unwrap();
        let mut response = Vec::new();
        stream.read_to_end(&mut response).await.unwrap();
        let header_end = response
            .windows(4)
            .position(|window| window == b"\r\n\r\n")
            .expect("HTTP response headers");
        let headers = std::str::from_utf8(&response[..header_end]).unwrap();
        let status = headers
            .lines()
            .next()
            .and_then(|line| line.split_whitespace().nth(1))
            .and_then(|value| value.parse().ok())
            .expect("HTTP status");
        (status, response[header_end + 4..].to_vec())
    }
}
