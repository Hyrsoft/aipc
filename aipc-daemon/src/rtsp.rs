use crate::config::RtspConfig;
use crate::model::ServerEvent;
use crate::preview::{PreviewFrame, VideoHub, annex_b_nals};
use anyhow::{Context, bail};
use base64::Engine;
use bytes::{Bytes, BytesMut};
use serde::Serialize;
use serde_json::json;
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncReadExt, AsyncWriteExt, ReadHalf, WriteHalf};
use tokio::net::{TcpListener, TcpStream, UdpSocket};
use tokio::sync::{Mutex, broadcast, watch};
use uuid::Uuid;

#[derive(Debug, Clone, Serialize)]
pub struct RtspStatus {
    pub enabled: bool,
    pub listening: bool,
    pub bind: String,
    pub path: String,
    pub clients: usize,
    pub max_clients: usize,
    pub last_error: Option<String>,
}

#[derive(Clone)]
pub struct RtspServer {
    config: RtspConfig,
    listening: Arc<AtomicBool>,
    clients: Arc<AtomicUsize>,
    last_error: Arc<std::sync::RwLock<Option<String>>>,
    shutdown: watch::Sender<bool>,
}

impl RtspServer {
    pub async fn start(
        config: RtspConfig,
        hub: VideoHub,
        events: broadcast::Sender<ServerEvent>,
    ) -> anyhow::Result<Self> {
        let (shutdown, shutdown_rx) = watch::channel(false);
        let server = Self {
            config: config.clone(),
            listening: Arc::new(AtomicBool::new(false)),
            clients: Arc::new(AtomicUsize::new(0)),
            last_error: Arc::new(std::sync::RwLock::new(None)),
            shutdown,
        };
        if !config.enabled {
            return Ok(server);
        }
        let listener = TcpListener::bind(&config.bind)
            .await
            .with_context(|| format!("bind RTSP server at {}", config.bind))?;
        server.listening.store(true, Ordering::Release);
        let task_server = server.clone();
        tokio::spawn(async move {
            run_listener(listener, config, hub, events, task_server, shutdown_rx).await;
        });
        Ok(server)
    }

    pub fn status(&self) -> RtspStatus {
        RtspStatus {
            enabled: self.config.enabled,
            listening: self.listening.load(Ordering::Acquire),
            bind: self.config.bind.clone(),
            path: self.config.path.clone(),
            clients: self.clients.load(Ordering::Acquire),
            max_clients: self.config.max_clients,
            last_error: self.last_error.read().unwrap().clone(),
        }
    }

    pub async fn shutdown(&self) {
        let _ = self.shutdown.send(true);
        self.listening.store(false, Ordering::Release);
    }
}

async fn run_listener(
    listener: TcpListener,
    config: RtspConfig,
    hub: VideoHub,
    events: broadcast::Sender<ServerEvent>,
    server: RtspServer,
    mut shutdown: watch::Receiver<bool>,
) {
    loop {
        tokio::select! {
            _ = shutdown.changed() => break,
            accepted = listener.accept() => match accepted {
                Ok((stream, peer)) => {
                    if server.clients.fetch_add(1, Ordering::AcqRel) >= config.max_clients {
                        server.clients.fetch_sub(1, Ordering::AcqRel);
                        tokio::spawn(async move {
                            let mut stream = stream;
                            let _ = stream.write_all(b"RTSP/1.0 453 Not Enough Bandwidth\r\nCSeq: 0\r\n\r\n").await;
                        });
                        continue;
                    }
                    let config = config.clone();
                    let hub = hub.clone();
                    let events = events.clone();
                    let clients = server.clients.clone();
                    tokio::spawn(async move {
                        let result = handle_client(stream, peer, config, hub).await;
                        clients.fetch_sub(1, Ordering::AcqRel);
                        let _ = events.send(ServerEvent::new("rtsp", json!({
                            "action": "client_disconnected", "peer": peer.to_string(),
                            "error": result.err().map(|error| error.to_string())
                        })));
                    });
                }
                Err(error) => {
                    *server.last_error.write().unwrap() = Some(error.to_string());
                    break;
                }
            }
        }
    }
    server.listening.store(false, Ordering::Release);
}

struct Request {
    method: String,
    uri: String,
    headers: HashMap<String, String>,
}

enum Transport {
    Tcp {
        rtp_channel: u8,
        writer: Arc<Mutex<WriteHalf<TcpStream>>>,
    },
    Udp {
        rtp: UdpSocket,
        rtcp: UdpSocket,
        rtp_target: SocketAddr,
        rtcp_target: SocketAddr,
    },
}

async fn handle_client(
    stream: TcpStream,
    peer: SocketAddr,
    config: RtspConfig,
    hub: VideoHub,
) -> anyhow::Result<()> {
    let (mut reader, writer) = tokio::io::split(stream);
    let writer = Arc::new(Mutex::new(writer));
    let session = Uuid::new_v4().simple().to_string();
    let mut transport: Option<Transport> = None;
    let mut play_stop: Option<tokio::sync::oneshot::Sender<()>> = None;
    loop {
        let Some(request) = read_request(&mut reader).await? else {
            break;
        };
        let cseq = request
            .headers
            .get("cseq")
            .map(String::as_str)
            .unwrap_or("0");
        let path_ok = request.uri.ends_with(&config.path)
            || request
                .uri
                .contains(&format!("{}/", config.path.trim_end_matches('/')))
            || request.uri == config.path
            || request.method == "OPTIONS";
        if !path_ok {
            respond(&writer, 404, "Not Found", cseq, &[], None).await?;
            continue;
        }
        match request.method.as_str() {
            "OPTIONS" => {
                respond(
                    &writer,
                    200,
                    "OK",
                    cseq,
                    &[(
                        "Public",
                        "OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, TEARDOWN",
                    )],
                    None,
                )
                .await?;
            }
            "DESCRIBE" => {
                let Some(codec) = hub.codec_config() else {
                    respond(&writer, 503, "Service Unavailable", cseq, &[], None).await?;
                    continue;
                };
                let profile = if codec.sps.len() >= 4 {
                    format!(
                        "{:02X}{:02X}{:02X}",
                        codec.sps[1], codec.sps[2], codec.sps[3]
                    )
                } else {
                    "42E01F".into()
                };
                let sps = base64::engine::general_purpose::STANDARD.encode(&codec.sps);
                let pps = base64::engine::general_purpose::STANDARD.encode(&codec.pps);
                let body = format!(
                    "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=AIPC\r\nt=0 0\r\na=control:*\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id={profile};sprop-parameter-sets={sps},{pps}\r\na=control:trackID=0\r\n"
                );
                respond(
                    &writer,
                    200,
                    "OK",
                    cseq,
                    &[("Content-Type", "application/sdp")],
                    Some(body.as_bytes()),
                )
                .await?;
            }
            "SETUP" => {
                let Some(value) = request.headers.get("transport") else {
                    respond(&writer, 400, "Bad Request", cseq, &[], None).await?;
                    continue;
                };
                if value.to_ascii_lowercase().contains("rtp/avp/tcp") {
                    let (first, second) = parse_interleaved(value).unwrap_or((0, 1));
                    transport = Some(Transport::Tcp {
                        rtp_channel: first,
                        writer: writer.clone(),
                    });
                    let header = format!("RTP/AVP/TCP;unicast;interleaved={first}-{second}");
                    respond_owned(
                        &writer,
                        200,
                        "OK",
                        cseq,
                        vec![("Transport", header), ("Session", session.clone())],
                        None,
                    )
                    .await?;
                } else if let Some((client_rtp, client_rtcp)) = parse_client_ports(value) {
                    let rtp = UdpSocket::bind("0.0.0.0:0").await?;
                    let rtcp = UdpSocket::bind("0.0.0.0:0").await?;
                    let server_rtp = rtp.local_addr()?.port();
                    let server_rtcp = rtcp.local_addr()?.port();
                    let rtp_target = SocketAddr::new(peer.ip(), client_rtp);
                    let rtcp_target = SocketAddr::new(peer.ip(), client_rtcp);
                    transport = Some(Transport::Udp {
                        rtp,
                        rtcp,
                        rtp_target,
                        rtcp_target,
                    });
                    let header = format!(
                        "RTP/AVP;unicast;client_port={client_rtp}-{client_rtcp};server_port={server_rtp}-{server_rtcp}"
                    );
                    respond_owned(
                        &writer,
                        200,
                        "OK",
                        cseq,
                        vec![("Transport", header), ("Session", session.clone())],
                        None,
                    )
                    .await?;
                } else {
                    respond(&writer, 461, "Unsupported Transport", cseq, &[], None).await?;
                }
            }
            "PLAY" => {
                let Some(selected) = transport.take() else {
                    respond(
                        &writer,
                        455,
                        "Method Not Valid in This State",
                        cseq,
                        &[],
                        None,
                    )
                    .await?;
                    continue;
                };
                let (stop_tx, stop_rx) = tokio::sync::oneshot::channel();
                play_stop = Some(stop_tx);
                let receiver = hub.subscribe();
                let codec = hub.codec_config();
                tokio::spawn(stream_rtp(receiver, codec, selected, config.mtu, stop_rx));
                respond_owned(
                    &writer,
                    200,
                    "OK",
                    cseq,
                    vec![("Session", session.clone()), ("Range", "npt=0.000-".into())],
                    None,
                )
                .await?;
            }
            "GET_PARAMETER" => {
                respond_owned(
                    &writer,
                    200,
                    "OK",
                    cseq,
                    vec![("Session", session.clone())],
                    None,
                )
                .await?;
            }
            "TEARDOWN" => {
                if let Some(stop) = play_stop.take() {
                    let _ = stop.send(());
                }
                respond_owned(
                    &writer,
                    200,
                    "OK",
                    cseq,
                    vec![("Session", session.clone())],
                    None,
                )
                .await?;
                break;
            }
            _ => respond(&writer, 405, "Method Not Allowed", cseq, &[], None).await?,
        }
    }
    if let Some(stop) = play_stop {
        let _ = stop.send(());
    }
    Ok(())
}

async fn read_request(reader: &mut ReadHalf<TcpStream>) -> anyhow::Result<Option<Request>> {
    let mut byte = [0_u8; 1];
    loop {
        let count = reader.read(&mut byte).await?;
        if count == 0 {
            return Ok(None);
        }
        if byte[0] != b'$' {
            break;
        }
        let mut header = [0_u8; 3];
        reader.read_exact(&mut header).await?;
        let length = u16::from_be_bytes([header[1], header[2]]) as usize;
        let mut payload = vec![0_u8; length];
        reader.read_exact(&mut payload).await?;
    }
    let mut data = Vec::with_capacity(1024);
    data.push(byte[0]);
    while data.len() < 16 * 1024 {
        let count = reader.read(&mut byte).await?;
        if count == 0 {
            return Ok(None);
        }
        data.push(byte[0]);
        if data.ends_with(b"\r\n\r\n") {
            break;
        }
    }
    if !data.ends_with(b"\r\n\r\n") {
        bail!("RTSP request header too large");
    }
    let text = std::str::from_utf8(&data)?;
    let mut lines = text.split("\r\n");
    let first = lines.next().unwrap_or_default();
    let mut parts = first.split_whitespace();
    let method = parts.next().unwrap_or_default().to_string();
    let uri = parts.next().unwrap_or_default().to_string();
    let mut headers = HashMap::new();
    for line in lines {
        if let Some((name, value)) = line.split_once(':') {
            headers.insert(name.trim().to_ascii_lowercase(), value.trim().to_string());
        }
    }
    if let Some(length) = headers
        .get("content-length")
        .and_then(|value| value.parse::<usize>().ok())
    {
        let mut body = vec![0_u8; length];
        reader.read_exact(&mut body).await?;
    }
    Ok(Some(Request {
        method,
        uri,
        headers,
    }))
}

async fn respond(
    writer: &Arc<Mutex<WriteHalf<TcpStream>>>,
    code: u16,
    reason: &str,
    cseq: &str,
    headers: &[(&str, &str)],
    body: Option<&[u8]>,
) -> anyhow::Result<()> {
    respond_owned(
        writer,
        code,
        reason,
        cseq,
        headers
            .iter()
            .map(|(a, b)| (*a, (*b).to_string()))
            .collect(),
        body,
    )
    .await
}

async fn respond_owned(
    writer: &Arc<Mutex<WriteHalf<TcpStream>>>,
    code: u16,
    reason: &str,
    cseq: &str,
    headers: Vec<(&str, String)>,
    body: Option<&[u8]>,
) -> anyhow::Result<()> {
    let mut response =
        format!("RTSP/1.0 {code} {reason}\r\nCSeq: {cseq}\r\nServer: aipc-daemon\r\n");
    for (name, value) in headers {
        response.push_str(&format!("{name}: {value}\r\n"));
    }
    if let Some(body) = body {
        response.push_str(&format!("Content-Length: {}\r\n", body.len()));
    }
    response.push_str("\r\n");
    let mut writer = writer.lock().await;
    writer.write_all(response.as_bytes()).await?;
    if let Some(body) = body {
        writer.write_all(body).await?;
    }
    Ok(())
}

async fn stream_rtp(
    mut receiver: broadcast::Receiver<Arc<PreviewFrame>>,
    codec: Option<crate::preview::CodecConfig>,
    transport: Transport,
    mtu: usize,
    mut stop: tokio::sync::oneshot::Receiver<()>,
) {
    let mut sequence = (SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .subsec_nanos()
        & 0xffff) as u16;
    let ssrc = sequence as u32 * 65537 + 0x4150;
    let mut packets_sent = 0_u32;
    let mut octets_sent = 0_u32;
    let mut last_sr = tokio::time::Instant::now();
    let mut waiting_keyframe = true;
    loop {
        let frame = tokio::select! {
            _ = &mut stop => break,
            result = receiver.recv() => match result { Ok(frame) => frame, Err(_) => { waiting_keyframe = true; continue; } }
        };
        if let Some(codec) = &codec {
            if frame.info.generation != codec.info.generation {
                break;
            }
        }
        if waiting_keyframe && !frame.keyframe {
            continue;
        }
        if waiting_keyframe {
            if let Some(codec) = &codec {
                let stap = stap_a(&codec.sps, &codec.pps);
                let timestamp = rtp_timestamp(frame.pts);
                let packet = rtp_packet(&stap, sequence, timestamp, ssrc, false);
                if send_rtp(&transport, &packet).await.is_err() {
                    break;
                }
                sequence = sequence.wrapping_add(1);
            }
            waiting_keyframe = false;
        }
        let payloads = packetize_h264(&frame.data, mtu.saturating_sub(12));
        let timestamp = rtp_timestamp(frame.pts);
        let payload_count = payloads.len();
        for (index, payload) in payloads.into_iter().enumerate() {
            let marker = index + 1 == payload_count;
            let packet = rtp_packet(&payload, sequence, timestamp, ssrc, marker);
            octets_sent = octets_sent.wrapping_add(payload.len() as u32);
            packets_sent = packets_sent.wrapping_add(1);
            if send_rtp(&transport, &packet).await.is_err() {
                return;
            }
            sequence = sequence.wrapping_add(1);
        }
        if last_sr.elapsed() >= Duration::from_secs(5) {
            let report = rtcp_sender_report(ssrc, timestamp, packets_sent, octets_sent);
            if send_rtcp(&transport, &report).await.is_err() {
                break;
            }
            last_sr = tokio::time::Instant::now();
        }
    }
}

async fn send_rtp(transport: &Transport, packet: &[u8]) -> std::io::Result<()> {
    match transport {
        Transport::Tcp {
            rtp_channel,
            writer,
        } => {
            let mut framed = Vec::with_capacity(packet.len() + 4);
            framed.extend_from_slice(&[b'$', *rtp_channel]);
            framed.extend_from_slice(&(packet.len() as u16).to_be_bytes());
            framed.extend_from_slice(packet);
            writer.lock().await.write_all(&framed).await
        }
        Transport::Udp {
            rtp, rtp_target, ..
        } => rtp.send_to(packet, rtp_target).await.map(|_| ()),
    }
}

async fn send_rtcp(transport: &Transport, packet: &[u8]) -> std::io::Result<()> {
    match transport {
        Transport::Tcp {
            rtp_channel,
            writer,
        } => {
            let mut framed = Vec::with_capacity(packet.len() + 4);
            framed.extend_from_slice(&[b'$', rtp_channel.saturating_add(1)]);
            framed.extend_from_slice(&(packet.len() as u16).to_be_bytes());
            framed.extend_from_slice(packet);
            writer.lock().await.write_all(&framed).await
        }
        Transport::Udp {
            rtcp, rtcp_target, ..
        } => rtcp.send_to(packet, rtcp_target).await.map(|_| ()),
    }
}

fn parse_interleaved(value: &str) -> Option<(u8, u8)> {
    let value = value
        .split(';')
        .find(|part| part.trim().starts_with("interleaved="))?
        .trim();
    let ports = value.strip_prefix("interleaved=")?;
    let (a, b) = ports.split_once('-')?;
    Some((a.parse().ok()?, b.parse().ok()?))
}

fn parse_client_ports(value: &str) -> Option<(u16, u16)> {
    let value = value
        .split(';')
        .find(|part| part.trim().starts_with("client_port="))?
        .trim();
    let ports = value.strip_prefix("client_port=")?;
    let (a, b) = ports.split_once('-')?;
    Some((a.parse().ok()?, b.parse().ok()?))
}

fn rtp_timestamp(pts_us: u64) -> u32 {
    (pts_us.saturating_mul(90) / 1000) as u32
}

fn rtp_packet(payload: &[u8], sequence: u16, timestamp: u32, ssrc: u32, marker: bool) -> Bytes {
    let mut packet = BytesMut::with_capacity(12 + payload.len());
    packet.extend_from_slice(&[0x80, 96 | if marker { 0x80 } else { 0 }]);
    packet.extend_from_slice(&sequence.to_be_bytes());
    packet.extend_from_slice(&timestamp.to_be_bytes());
    packet.extend_from_slice(&ssrc.to_be_bytes());
    packet.extend_from_slice(payload);
    packet.freeze()
}

pub fn packetize_h264(data: &Bytes, max_payload: usize) -> Vec<Bytes> {
    let mut output = Vec::new();
    let nals: Vec<Bytes> = annex_b_nals(data)
        .into_iter()
        .filter_map(|(kind, nal)| {
            if matches!(kind, 7 | 8 | 9) {
                return None;
            }
            Some(if nal.starts_with(&[0, 0, 0, 1]) {
                nal.slice(4..)
            } else {
                nal.slice(3..)
            })
        })
        .collect();
    for nal in nals {
        if nal.len() <= max_payload {
            output.push(nal);
            continue;
        }
        if nal.is_empty() || max_payload <= 2 {
            continue;
        }
        let indicator = (nal[0] & 0xe0) | 28;
        let kind = nal[0] & 0x1f;
        let chunk_size = max_payload - 2;
        let chunks: Vec<&[u8]> = nal[1..].chunks(chunk_size).collect();
        for (index, chunk) in chunks.iter().enumerate() {
            let mut payload = BytesMut::with_capacity(chunk.len() + 2);
            payload.extend_from_slice(&[
                indicator,
                kind | if index == 0 { 0x80 } else { 0 }
                    | if index + 1 == chunks.len() { 0x40 } else { 0 },
            ]);
            payload.extend_from_slice(chunk);
            output.push(payload.freeze());
        }
    }
    output
}

fn stap_a(sps: &Bytes, pps: &Bytes) -> Bytes {
    let mut payload = BytesMut::with_capacity(5 + sps.len() + pps.len());
    payload.extend_from_slice(&[0x78]);
    payload.extend_from_slice(&(sps.len() as u16).to_be_bytes());
    payload.extend_from_slice(sps);
    payload.extend_from_slice(&(pps.len() as u16).to_be_bytes());
    payload.extend_from_slice(pps);
    payload.freeze()
}

fn rtcp_sender_report(ssrc: u32, rtp_timestamp: u32, packets: u32, octets: u32) -> Bytes {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let ntp_seconds = now.as_secs().wrapping_add(2_208_988_800) as u32;
    let ntp_fraction = ((now.subsec_nanos() as u64) << 32) / 1_000_000_000;
    let mut packet = BytesMut::with_capacity(28);
    packet.extend_from_slice(&[0x80, 200, 0, 6]);
    packet.extend_from_slice(&ssrc.to_be_bytes());
    packet.extend_from_slice(&ntp_seconds.to_be_bytes());
    packet.extend_from_slice(&(ntp_fraction as u32).to_be_bytes());
    packet.extend_from_slice(&rtp_timestamp.to_be_bytes());
    packet.extend_from_slice(&packets.to_be_bytes());
    packet.extend_from_slice(&octets.to_be_bytes());
    packet.freeze()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fragments_large_nal_as_fu_a() {
        let mut source = vec![0, 0, 0, 1, 0x65];
        source.extend(std::iter::repeat_n(7, 30));
        let packets = packetize_h264(&Bytes::from(source), 10);
        assert!(packets.len() > 1);
        assert_ne!(packets[0][1] & 0x80, 0);
        assert_ne!(packets.last().unwrap()[1] & 0x40, 0);
    }

    #[test]
    fn parses_transports() {
        assert_eq!(
            parse_interleaved("RTP/AVP/TCP;unicast;interleaved=2-3"),
            Some((2, 3))
        );
        assert_eq!(
            parse_client_ports("RTP/AVP;unicast;client_port=5000-5001"),
            Some((5000, 5001))
        );
    }
}
