import { PreviewController, initialPreviewSnapshot, type PreviewSnapshot, type PreviewStreamInfo } from './preview'

interface WebRtcStatus {
  enabled: boolean
  listening: boolean
  video_available: boolean
  audio_available: boolean
}

interface PreviewStatusResponse {
  video?: { stream?: PreviewStreamInfo | null }
}

interface SessionAnswer {
  id: string
  type: 'answer'
  sdp: string
}

interface WebRtcDependencies {
  peerFactory: () => RTCPeerConnection
  mediaStreamFactory: () => MediaStream
  fetchStatus: () => Promise<{ webrtc: WebRtcStatus; preview: PreviewStatusResponse }>
  createSession: (offer: RTCSessionDescriptionInit) => Promise<SessionAnswer>
  deleteSession: (id: string) => Promise<void>
  setTimer: typeof window.setTimeout
  clearTimer: typeof window.clearTimeout
  now: () => number
}

interface PreviewControl {
  connect(video: HTMLVideoElement): void
  disconnect(): void
  destroy(): void
  setVolume(value: number): void
  setMuted(muted: boolean): void
}

export class AdaptivePreviewController {
  private readonly fallback: PreviewControl
  private readonly webrtc: WebRtcPreviewController
  private active: 'webrtc' | 'fallback' | null = null
  private video: HTMLVideoElement | null = null
  private desired = false
  private fallbackStarted = false
  private volume = 1
  private muted = false

  constructor(
    onUpdate: (snapshot: PreviewSnapshot) => void,
    dependencies = browserWebRtcDependencies(),
    fallbackFactory: (update: (snapshot: PreviewSnapshot) => void) => PreviewControl = (update) => new PreviewController(update),
  ) {
    this.fallback = fallbackFactory((snapshot) => {
      if (this.active === 'fallback') onUpdate(snapshot)
    })
    this.webrtc = new WebRtcPreviewController(
      (snapshot) => {
        if (this.active === 'webrtc') {
          this.volume = snapshot.volume
          this.muted = snapshot.muted
          onUpdate(snapshot)
        }
      },
      () => this.startFallback(),
      dependencies,
    )
  }

  connect(video: HTMLVideoElement) {
    this.video = video
    this.desired = true
    if (this.active) return
    this.active = 'webrtc'
    this.fallbackStarted = false
    void this.webrtc.connect(video)
  }

  disconnect() {
    this.desired = false
    this.webrtc.disconnect()
    this.fallback.disconnect()
    this.active = null
    this.fallbackStarted = false
  }

  destroy() {
    this.disconnect()
    this.fallback.destroy()
    this.video = null
  }

  setVolume(value: number) {
    this.volume = Math.max(0, Math.min(1, value))
    if (this.active === 'fallback') this.fallback.setVolume(value)
    else this.webrtc.setVolume(value)
  }

  setMuted(muted: boolean) {
    this.muted = muted
    if (this.active === 'fallback') this.fallback.setMuted(muted)
    else this.webrtc.setMuted(muted)
  }

  private startFallback() {
    if (!this.desired || !this.video || this.fallbackStarted) return
    this.fallbackStarted = true
    this.webrtc.disconnect()
    this.active = 'fallback'
    this.fallback.connect(this.video)
    this.fallback.setVolume(this.volume)
    this.fallback.setMuted(this.muted)
  }
}

class WebRtcPreviewController {
  private pc: RTCPeerConnection | null = null
  private aiChannel: RTCDataChannel | null = null
  private video: HTMLVideoElement | null = null
  private sessionId: string | null = null
  private desired = false
  private statsTimer: number | null = null
  private firstVideoTimer: number | null = null
  private snapshot = initialPreviewSnapshot()
  private previousStats = new Map<string, { bytes: number; frames: number; at: number }>()
  private readonly aiToggleHandler = () => this.sendAiEnabled()

  constructor(
    private readonly onUpdate: (snapshot: PreviewSnapshot) => void,
    private readonly onFallback: () => void,
    private readonly dependencies: WebRtcDependencies,
  ) {}

  async connect(video: HTMLVideoElement) {
    if (this.pc || !this.dependencies.peerFactory) return
    this.video = video
    this.desired = true
    this.patch({ state: 'connecting', error: '', muted: true })
    video.muted = true
    video.volume = this.snapshot.volume
    try {
      const { webrtc, preview } = await this.dependencies.fetchStatus()
      if (!this.desired) return
      if (!webrtc.enabled || !webrtc.listening || !webrtc.video_available) {
        throw new Error('WebRTC 服务或视频尚未就绪')
      }
      this.patch({ stream: preview.video?.stream || null })
      const pc = this.dependencies.peerFactory()
      this.pc = pc
      const stream = this.dependencies.mediaStreamFactory()
      if (typeof pc.createDataChannel === 'function') {
        const aiChannel = pc.createDataChannel('aipc-ai', { ordered: false, maxRetransmits: 0 })
        this.aiChannel = aiChannel
        if (typeof window !== 'undefined') window.addEventListener('aipc-ai-toggle', this.aiToggleHandler)
        aiChannel.onopen = () => this.sendAiEnabled()
        aiChannel.onmessage = (event) => {
          if (typeof event.data !== 'string' || typeof window === 'undefined') return
          try {
            window.dispatchEvent(new CustomEvent('aipc-ai-metadata', { detail: JSON.parse(event.data) }))
          } catch { /* malformed metadata is ignored */ }
        }
      }
      pc.addTransceiver('video', { direction: 'recvonly' })
      if (webrtc.audio_available) pc.addTransceiver('audio', { direction: 'recvonly' })
      pc.ontrack = (event) => {
        if (!this.desired || this.pc !== pc) return
        if (!stream.getTracks().some((track) => track.id === event.track.id)) stream.addTrack(event.track)
        if (this.video) {
          this.video.srcObject = event.streams[0] || stream
          void this.video.play().catch(() => undefined)
        }
        if (event.track.kind === 'video') {
          this.clearFirstVideoTimer()
          this.patch({ state: 'live', error: '' })
        } else {
          this.patch({ audioState: 'live', audioError: '' })
        }
      }
      pc.onconnectionstatechange = () => {
        if (!this.desired || this.pc !== pc) return
        if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected' || pc.connectionState === 'closed') {
          this.fail(`WebRTC ${pc.connectionState}`)
        } else if (pc.connectionState === 'connected') {
          this.patch({ state: this.snapshot.state === 'live' ? 'live' : 'waiting' })
        }
      }
      const offer = await pc.createOffer()
      await pc.setLocalDescription(offer)
      await waitForIceGathering(pc, this.dependencies)
      if (!pc.localDescription) throw new Error('浏览器未生成 WebRTC offer')
      const answer = await this.dependencies.createSession(pc.localDescription)
      if (!this.desired || this.pc !== pc) {
        await this.dependencies.deleteSession(answer.id).catch(() => undefined)
        return
      }
      this.sessionId = answer.id
      await pc.setRemoteDescription({ type: answer.type, sdp: answer.sdp })
      this.patch({ state: 'waiting' })
      this.firstVideoTimer = this.dependencies.setTimer(() => this.fail('WebRTC 首帧超时'), 8000)
      this.startStats()
    } catch (cause) {
      this.fail(String(cause))
    }
  }

  disconnect() {
    this.desired = false
    this.clearTimers()
    const id = this.sessionId
    this.sessionId = null
    if (id) void this.dependencies.deleteSession(id).catch(() => undefined)
    this.pc?.close()
    this.pc = null
    this.aiChannel = null
    if (typeof window !== 'undefined') window.removeEventListener('aipc-ai-toggle', this.aiToggleHandler)
    this.previousStats.clear()
    if (this.video) this.video.srcObject = null
    this.patch({ state: 'disconnected', stream: null, audioState: 'waiting' })
  }

  setVolume(value: number) {
    const volume = Math.max(0, Math.min(1, value))
    if (this.video) this.video.volume = volume
    this.patch({ volume })
  }

  setMuted(muted: boolean) {
    if (this.video) {
      this.video.muted = muted
      if (!muted) void this.video.play().catch(() => undefined)
    }
    this.patch({ muted })
  }

  private sendAiEnabled() {
    if (this.aiChannel?.readyState !== 'open') return
    const enabled = typeof window === 'undefined' || window.localStorage.getItem('aipc-ai-overlay') !== 'off'
    this.aiChannel.send(JSON.stringify({ enabled }))
  }

  private fail(message: string) {
    if (!this.desired) return
    this.patch({ state: 'error', error: message, audioState: 'error', audioError: message })
    this.onFallback()
  }

  private startStats() {
    this.stopStats()
    const tick = async () => {
      const pc = this.pc
      if (!pc || !this.desired) return
      try {
        const reports = await pc.getStats()
        let videoKbps = 0
        let audioKbps = 0
        let fps = 0
        let lost = 0
        let audioPackets = 0
        const now = this.dependencies.now()
        reports.forEach((report) => {
          if (report.type !== 'inbound-rtp') return
          const kind = report.kind || report.mediaType
          const bytes = Number(report.bytesReceived || 0)
          const frames = Number(report.framesDecoded || 0)
          const previous = this.previousStats.get(report.id)
          if (previous) {
            const seconds = Math.max((now - previous.at) / 1000, 0.001)
            const kbps = Math.max(0, bytes - previous.bytes) * 8 / seconds / 1000
            if (kind === 'video') {
              videoKbps += kbps
              fps += Math.max(0, frames - previous.frames) / seconds
            } else if (kind === 'audio') audioKbps += kbps
          }
          this.previousStats.set(report.id, { bytes, frames, at: now })
          lost += Number(report.packetsLost || 0)
          if (kind === 'audio') audioPackets += Number(report.packetsReceived || 0)
        })
        this.patch({
          bitrateKbps: videoKbps,
          audioBitrateKbps: audioKbps,
          receivedFps: fps,
          droppedFrames: lost,
          audioPackets,
        })
      } catch { /* connection state handler owns recovery */ }
      if (this.pc && this.desired) this.statsTimer = this.dependencies.setTimer(tick, 1000)
    }
    this.statsTimer = this.dependencies.setTimer(tick, 1000)
  }

  private stopStats() {
    if (this.statsTimer !== null) this.dependencies.clearTimer(this.statsTimer)
    this.statsTimer = null
  }

  private clearFirstVideoTimer() {
    if (this.firstVideoTimer !== null) this.dependencies.clearTimer(this.firstVideoTimer)
    this.firstVideoTimer = null
  }

  private clearTimers() {
    this.stopStats()
    this.clearFirstVideoTimer()
  }

  private patch(update: Partial<PreviewSnapshot>) {
    this.snapshot = { ...this.snapshot, ...update }
    this.onUpdate({ ...this.snapshot })
  }
}

async function waitForIceGathering(pc: RTCPeerConnection, dependencies: WebRtcDependencies) {
  if (pc.iceGatheringState === 'complete') return
  await new Promise<void>((resolve) => {
    const timeout = dependencies.setTimer(done, 2000)
    function done() {
      dependencies.clearTimer(timeout)
      pc.removeEventListener('icegatheringstatechange', changed)
      resolve()
    }
    function changed() {
      if (pc.iceGatheringState === 'complete') done()
    }
    pc.addEventListener('icegatheringstatechange', changed)
  })
}

function browserWebRtcDependencies(): WebRtcDependencies {
  return {
    peerFactory: () => new RTCPeerConnection({ bundlePolicy: 'max-bundle' }),
    mediaStreamFactory: () => new MediaStream(),
    fetchStatus: async () => {
      const [webrtc, preview] = await Promise.all([
        fetch('/api/v1/webrtc/status').then((response) => response.json()),
        fetch('/api/v1/preview/status').then((response) => response.json()),
      ])
      return { webrtc, preview }
    },
    createSession: async (offer) => {
      const response = await fetch('/api/v1/webrtc/sessions', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(offer),
      })
      const body = await response.json().catch(() => ({}))
      if (!response.ok) throw new Error(body?.error?.message || `HTTP ${response.status}`)
      return body
    },
    deleteSession: async (id) => {
      const response = await fetch(`/api/v1/webrtc/sessions/${encodeURIComponent(id)}`, { method: 'DELETE' })
      if (!response.ok && response.status !== 404) throw new Error(`HTTP ${response.status}`)
    },
    setTimer: window.setTimeout.bind(window),
    clearTimer: window.clearTimeout.bind(window),
    now: () => performance.now(),
  }
}

export type { PreviewControl, WebRtcDependencies }
