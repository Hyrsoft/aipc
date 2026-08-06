import JMuxer from 'jmuxer'

export interface PreviewStreamInfo {
  generation: string
  codec: string
  format: string
  width: number
  height: number
  fps: number
}

export interface PreviewAudioInfo {
  generation: string
  codec: string
  sample_rate: number
  channels: number
  bit_width: number
  bitrate: number
}

export interface PreviewSnapshot {
  state: 'disconnected' | 'connecting' | 'waiting' | 'live' | 'error' | 'unsupported'
  stream: PreviewStreamInfo | null
  receivedFps: number
  bitrateKbps: number
  bytesReceived: number
  droppedFrames: number
  reconnects: number
  error: string
  audioStream: PreviewAudioInfo | null
  audioState: 'disabled' | 'waiting' | 'live' | 'error'
  audioPackets: number
  audioBitrateKbps: number
  audioError: string
  volume: number
  muted: boolean
}

interface SocketLike {
  binaryType: BinaryType
  onopen: ((event: Event) => void) | null
  onmessage: ((event: MessageEvent) => void) | null
  onclose: ((event: CloseEvent) => void) | null
  onerror: ((event: Event) => void) | null
  close(): void
}

interface MuxerLike {
  feed(data: { video: Uint8Array; duration?: number }): void
  destroy(): void
}

interface PreviewDependencies {
  socketFactory: (url: string) => SocketLike
  muxerFactory: (video: HTMLVideoElement, fps: number, onError: (message: string) => void,
    onMissing: (count: number) => void) => MuxerLike
  setTimer: typeof window.setTimeout
  clearTimer: typeof window.clearTimeout
  now: () => number
  mseSupported: () => boolean
  websocketUrl: () => string
}

const reconnectDelays = [1000, 2000, 4000, 8000]

export class PreviewController {
  private socket: SocketLike | null = null
  private muxer: MuxerLike | null = null
  private video: HTMLVideoElement | null = null
  private reconnectTimer: number | null = null
  private statsTimer: number | null = null
  private desired = false
  private reconnectAttempt = 0
  private intervalBytes = 0
  private intervalFrames = 0
  private intervalStarted = 0
  private readonly dependencies: PreviewDependencies
  private readonly onUpdate: (snapshot: PreviewSnapshot) => void
  private snapshot: PreviewSnapshot = initialPreviewSnapshot()
  private audioContext: AudioContext | null = null
  private audioGain: GainNode | null = null
  private audioSources = new Set<AudioBufferSourceNode>()
  private nextAudioTime = 0
  private intervalAudioBytes = 0

  constructor(onUpdate: (snapshot: PreviewSnapshot) => void, dependencies = browserDependencies()) {
    this.onUpdate = onUpdate
    this.dependencies = dependencies
  }

  connect(video: HTMLVideoElement) {
    this.video = video
    this.desired = true
    if (!this.dependencies.mseSupported()) {
      this.patch({ state: 'unsupported', error: '当前浏览器不支持 Media Source Extensions' })
      return
    }
    if (this.socket || this.snapshot.state === 'connecting') return
    this.openSocket()
  }

  disconnect() {
    this.desired = false
    this.clearReconnect()
    this.socket?.close()
    this.socket = null
    this.destroyMuxer()
    this.stopStats()
    this.resetAudio()
    this.patch({ state: 'disconnected', stream: null, receivedFps: 0, bitrateKbps: 0, error: '', audioState: 'waiting' })
  }

  destroy() {
    this.disconnect()
    this.video = null
    void this.audioContext?.close()
    this.audioContext = null
  }

  setVolume(value: number) {
    const volume = Math.max(0, Math.min(1, value))
    this.patch({ volume })
    if (this.audioGain) this.audioGain.gain.value = this.snapshot.muted ? 0 : volume
    if (this.audioContext?.state === 'suspended') void this.audioContext.resume()
  }

  setMuted(muted: boolean) {
    this.patch({ muted })
    if (this.audioGain) this.audioGain.gain.value = muted ? 0 : this.snapshot.volume
    if (this.audioContext?.state === 'suspended') void this.audioContext.resume()
  }

  private openSocket() {
    if (!this.video || !this.desired) return
    this.patch({ state: 'connecting', error: '' })
    const socket = this.dependencies.socketFactory(this.dependencies.websocketUrl())
    socket.binaryType = 'arraybuffer'
    socket.onopen = () => {
      this.reconnectAttempt = 0
      this.patch({ state: 'waiting', error: '' })
      this.startStats()
    }
    socket.onmessage = (event) => this.handleMessage(event)
    socket.onerror = () => this.patch({ state: 'error', error: '视频 WebSocket 连接错误' })
    socket.onclose = () => {
      if (this.socket === socket) this.socket = null
      this.destroyMuxer()
      this.stopStats()
      if (this.desired) this.scheduleReconnect()
      else this.patch({ state: 'disconnected' })
    }
    this.socket = socket
  }

  private handleMessage(event: MessageEvent) {
    if (typeof event.data === 'string') {
      const message = JSON.parse(event.data)
      if (message.type === 'reset') {
        this.destroyMuxer()
        this.resetAudio()
        this.patch({ state: 'waiting', audioStream: null, audioState: 'waiting' })
      } else if (message.type === 'stream') {
        const stream = message.stream as PreviewStreamInfo
        const changed = this.snapshot.stream?.generation !== stream.generation
          || this.snapshot.stream?.fps !== stream.fps
          || this.snapshot.stream?.width !== stream.width
          || this.snapshot.stream?.height !== stream.height
        if (changed) this.destroyMuxer()
        this.patch({ stream, state: 'waiting' })
        this.ensureMuxer(stream.fps)
      } else if (message.type === 'state' && message.state === 'stopped') {
        this.destroyMuxer()
        this.patch({ state: 'waiting', stream: null })
      } else if (message.type === 'audio_stream') {
        const audioStream = message.stream as PreviewAudioInfo
        this.patch({ audioStream, audioState: 'waiting', audioError: '' })
      }
      return
    }
    const buffer = event.data instanceof ArrayBuffer ? event.data : null
    if (!buffer || buffer.byteLength === 0) return
    const bytes = new Uint8Array(buffer)
    if (bytes.length >= 28 && bytes[0] === 0x41 && bytes[1] === 0x49 && bytes[2] === 0x50 && bytes[3] === 0x41) {
      this.handleAudioFrame(bytes)
      return
    }
    if (!this.snapshot.stream) return
    this.ensureMuxer(this.snapshot.stream.fps)
    this.muxer?.feed({
      video: new Uint8Array(buffer),
      duration: 1000 / this.snapshot.stream.fps,
    })
    this.intervalBytes += buffer.byteLength
    this.intervalFrames += 1
    this.snapshot.bytesReceived += buffer.byteLength
    this.patch({ state: 'live', bytesReceived: this.snapshot.bytesReceived })
  }

  private handleAudioFrame(frame: Uint8Array) {
    const stream = this.snapshot.audioStream
    if (!stream) return
    const length = new DataView(frame.buffer, frame.byteOffset, frame.byteLength).getUint32(8)
    if (length <= 0 || length > frame.byteLength - 28) {
      this.patch({ audioState: 'error', audioError: '无效的 AIPA 音频帧' })
      return
    }
    try {
      this.ensureAudio()
      if (!this.audioContext || !this.audioGain) return
      if (this.audioContext.state === 'suspended') void this.audioContext.resume()
      const payload = frame.subarray(28, 28 + length)
      const audioBuffer = this.audioContext.createBuffer(1, payload.length, stream.sample_rate)
      const channel = audioBuffer.getChannelData(0)
      for (let index = 0; index < payload.length; index += 1) channel[index] = decodeG711A(payload[index]) / 32768
      if (this.nextAudioTime > this.audioContext.currentTime + 0.5) this.resetScheduledAudio()
      const source = this.audioContext.createBufferSource()
      source.buffer = audioBuffer
      source.connect(this.audioGain)
      const startAt = Math.max(this.nextAudioTime, this.audioContext.currentTime + 0.04)
      source.start(startAt)
      this.nextAudioTime = startAt + audioBuffer.duration
      this.audioSources.add(source)
      source.onended = () => this.audioSources.delete(source)
      this.intervalAudioBytes += payload.length
      this.patch({ audioState: 'live', audioPackets: this.snapshot.audioPackets + 1, audioError: '' })
    } catch (cause) {
      this.patch({ audioState: 'error', audioError: String(cause) })
    }
  }

  private ensureAudio() {
    if (this.audioContext) return
    const Context = window.AudioContext || (window as typeof window & { webkitAudioContext?: typeof AudioContext }).webkitAudioContext
    if (!Context) throw new Error('浏览器不支持 Web Audio API')
    this.audioContext = new Context()
    this.audioGain = this.audioContext.createGain()
    this.audioGain.gain.value = this.snapshot.muted ? 0 : this.snapshot.volume
    this.audioGain.connect(this.audioContext.destination)
  }

  private resetScheduledAudio() {
    for (const source of this.audioSources) {
      try { source.stop() } catch { /* already stopped */ }
    }
    this.audioSources.clear()
    this.nextAudioTime = this.audioContext?.currentTime || 0
  }

  private resetAudio() {
    this.resetScheduledAudio()
    this.intervalAudioBytes = 0
  }

  private ensureMuxer(fps: number) {
    if (this.muxer || !this.video) return
    this.muxer = this.dependencies.muxerFactory(
      this.video,
      fps,
      (message) => this.patch({ state: 'error', error: message }),
      (count) => this.patch({ droppedFrames: this.snapshot.droppedFrames + count }),
    )
  }

  private destroyMuxer() {
    this.muxer?.destroy()
    this.muxer = null
  }

  private scheduleReconnect() {
    this.clearReconnect()
    const delay = reconnectDelays[Math.min(this.reconnectAttempt, reconnectDelays.length - 1)]
    this.reconnectAttempt += 1
    this.patch({ state: 'connecting', reconnects: this.snapshot.reconnects + 1 })
    this.reconnectTimer = this.dependencies.setTimer(() => {
      this.reconnectTimer = null
      this.openSocket()
    }, delay)
  }

  private clearReconnect() {
    if (this.reconnectTimer !== null) this.dependencies.clearTimer(this.reconnectTimer)
    this.reconnectTimer = null
  }

  private startStats() {
    this.stopStats()
    this.intervalBytes = 0
    this.intervalFrames = 0
    this.intervalStarted = this.dependencies.now()
    const tick = () => {
      const now = this.dependencies.now()
      const seconds = Math.max((now - this.intervalStarted) / 1000, 0.001)
      this.patch({
        receivedFps: this.intervalFrames / seconds,
        bitrateKbps: this.intervalBytes * 8 / seconds / 1000,
        audioBitrateKbps: this.intervalAudioBytes * 8 / seconds / 1000,
      })
      this.intervalBytes = 0
      this.intervalFrames = 0
      this.intervalAudioBytes = 0
      this.intervalStarted = now
      if (this.socket) this.statsTimer = this.dependencies.setTimer(tick, 1000)
    }
    this.statsTimer = this.dependencies.setTimer(tick, 1000)
  }

  private stopStats() {
    if (this.statsTimer !== null) this.dependencies.clearTimer(this.statsTimer)
    this.statsTimer = null
  }

  private patch(update: Partial<PreviewSnapshot>) {
    this.snapshot = { ...this.snapshot, ...update }
    this.onUpdate({ ...this.snapshot })
  }
}

export function initialPreviewSnapshot(): PreviewSnapshot {
  return {
    state: 'disconnected', stream: null, receivedFps: 0, bitrateKbps: 0,
    bytesReceived: 0, droppedFrames: 0, reconnects: 0, error: '',
    audioStream: null, audioState: 'waiting', audioPackets: 0, audioBitrateKbps: 0,
    audioError: '', volume: 1, muted: false,
  }
}

export function decodeG711A(encoded: number): number {
  const value = encoded ^ 0x55
  let sample = (value & 0x0f) << 4
  const segment = (value & 0x70) >> 4
  sample += 8
  if (segment >= 1) sample += 0x100
  if (segment > 1) sample <<= segment - 1
  return value & 0x80 ? sample : -sample
}

function browserDependencies(): PreviewDependencies {
  return {
    socketFactory: (url) => new WebSocket(url),
    muxerFactory: (video, fps, onError, onMissing) => new JMuxer({
      node: video, mode: 'video', flushingTime: 0, maxDelay: 200,
      clearBuffer: true, fps, debug: false,
      onError: (error) => onError(String(error)),
      onMissingVideoFrames: (count) => onMissing(Number(count) || 1),
    }),
    setTimer: window.setTimeout.bind(window),
    clearTimer: window.clearTimeout.bind(window),
    now: () => performance.now(),
    mseSupported: () => typeof MediaSource !== 'undefined',
    websocketUrl: () => {
      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
      return `${protocol}//${window.location.host}/api/v1/preview/ws`
    },
  }
}

export type { PreviewDependencies, SocketLike, MuxerLike }
