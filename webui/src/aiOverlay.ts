import type { AiDetection, AiMetadata } from './types'

interface Sample {
  at: number
  detection: AiDetection
}

interface Track {
  previous?: Sample
  current: Sample
}

const EXTRAPOLATE_MS = 150
const EXPIRE_MS = 300

export class AiOverlayController {
  private readonly tracks = new Map<number, Track>()
  private enabled = window.localStorage.getItem('aipc-ai-overlay') !== 'off'
  private source: EventSource | null = null
  private animation = 0
  private lastGeneration = ''
  private lastSequence = -1
  private readonly metadataHandler = (event: Event) => {
    this.consume((event as CustomEvent<AiMetadata>).detail)
  }

  constructor(
    private readonly video: HTMLVideoElement,
    private readonly canvas: HTMLCanvasElement,
  ) {
    window.addEventListener('aipc-ai-metadata', this.metadataHandler)
    this.connectSse()
    this.render = this.render.bind(this)
    this.animation = requestAnimationFrame(this.render)
  }

  isEnabled() { return this.enabled }

  setEnabled(enabled: boolean) {
    this.enabled = enabled
    window.localStorage.setItem('aipc-ai-overlay', enabled ? 'on' : 'off')
    window.dispatchEvent(new CustomEvent('aipc-ai-toggle', { detail: enabled }))
    if (!enabled) {
      this.tracks.clear()
      this.clear()
    }
  }

  destroy() {
    cancelAnimationFrame(this.animation)
    this.source?.close()
    window.removeEventListener('aipc-ai-metadata', this.metadataHandler)
    this.clear()
  }

  private connectSse() {
    this.source = new EventSource('/api/v1/ai/events')
    this.source.addEventListener('detections', (event) => {
      try { this.consume(JSON.parse((event as MessageEvent).data) as AiMetadata) } catch { /* retry on next event */ }
    })
  }

  private consume(metadata: AiMetadata) {
    if (!this.enabled || metadata.version !== 1 || !Array.isArray(metadata.detections)) return
    if (metadata.generation === this.lastGeneration && metadata.sequence <= this.lastSequence) return
    if (metadata.generation !== this.lastGeneration) {
      this.tracks.clear()
      this.lastGeneration = metadata.generation
    }
    this.lastSequence = metadata.sequence
    const at = performance.now()
    const present = new Set<number>()
    for (const detection of metadata.detections) {
      if (!validDetection(detection)) continue
      present.add(detection.track_id)
      const existing = this.tracks.get(detection.track_id)
      this.tracks.set(detection.track_id, {
        previous: existing?.current,
        current: { at, detection },
      })
    }
    for (const [id, track] of this.tracks) {
      if (!present.has(id) && at - track.current.at > EXPIRE_MS) this.tracks.delete(id)
    }
  }

  private render() {
    this.animation = requestAnimationFrame(this.render)
    this.resize()
    this.clear()
    if (!this.enabled || this.video.readyState < HTMLMediaElement.HAVE_CURRENT_DATA) return
    const context = this.canvas.getContext('2d')
    if (!context) return
    const now = performance.now()
    const viewport = containViewport(this.canvas.width, this.canvas.height, this.video.videoWidth, this.video.videoHeight)
    for (const [id, track] of this.tracks) {
      const age = now - track.current.at
      if (age > EXPIRE_MS) {
        this.tracks.delete(id)
        continue
      }
      const box = interpolated(track, now)
      const alpha = age <= EXTRAPOLATE_MS ? 1 : 1 - (age - EXTRAPOLATE_MS) / (EXPIRE_MS - EXTRAPOLATE_MS)
      drawDetection(context, box, viewport, Math.max(0, alpha))
    }
  }

  private resize() {
    const scale = window.devicePixelRatio || 1
    const width = Math.max(1, Math.round(this.canvas.clientWidth * scale))
    const height = Math.max(1, Math.round(this.canvas.clientHeight * scale))
    if (this.canvas.width !== width) this.canvas.width = width
    if (this.canvas.height !== height) this.canvas.height = height
  }

  private clear() {
    this.canvas.getContext('2d')?.clearRect(0, 0, this.canvas.width, this.canvas.height)
  }
}

function validDetection(value: AiDetection) {
  return Number.isFinite(value.track_id) && Number.isFinite(value.x) && Number.isFinite(value.y)
    && Number.isFinite(value.width) && Number.isFinite(value.height)
}

function interpolated(track: Track, now: number): AiDetection {
  const current = track.current
  const previous = track.previous
  if (!previous) return current.detection
  const interval = Math.max(1, current.at - previous.at)
  const factor = Math.min(1 + EXTRAPOLATE_MS / interval, Math.max(0, (now - previous.at) / interval))
  return {
    ...current.detection,
    x: lerp(previous.detection.x, current.detection.x, factor),
    y: lerp(previous.detection.y, current.detection.y, factor),
    width: lerp(previous.detection.width, current.detection.width, factor),
    height: lerp(previous.detection.height, current.detection.height, factor),
  }
}

function lerp(from: number, to: number, amount: number) { return from + (to - from) * amount }

function containViewport(canvasWidth: number, canvasHeight: number, videoWidth: number, videoHeight: number) {
  if (!videoWidth || !videoHeight) return { x: 0, y: 0, width: canvasWidth, height: canvasHeight }
  const scale = Math.min(canvasWidth / videoWidth, canvasHeight / videoHeight)
  const width = videoWidth * scale
  const height = videoHeight * scale
  return { x: (canvasWidth - width) / 2, y: (canvasHeight - height) / 2, width, height }
}

function drawDetection(
  context: CanvasRenderingContext2D,
  detection: AiDetection,
  viewport: { x: number; y: number; width: number; height: number },
  alpha: number,
) {
  const x = viewport.x + clamp(detection.x) * viewport.width
  const y = viewport.y + clamp(detection.y) * viewport.height
  const width = clamp(detection.width) * viewport.width
  const height = clamp(detection.height) * viewport.height
  if (width < 1 || height < 1) return
  const scale = window.devicePixelRatio || 1
  const label = `${detection.label || detection.class_id} ${(detection.confidence * 100).toFixed(0)}%`
  context.save()
  context.globalAlpha = alpha
  context.strokeStyle = '#48e0a8'
  context.fillStyle = 'rgba(15, 38, 30, .82)'
  context.lineWidth = Math.max(2, 2 * scale)
  context.font = `${Math.max(11, 11 * scale)}px ui-monospace, monospace`
  context.strokeRect(x, y, width, height)
  const textWidth = context.measureText(label).width + 10 * scale
  const textHeight = 19 * scale
  const textY = Math.max(viewport.y, y - textHeight)
  context.fillRect(x, textY, textWidth, textHeight)
  context.fillStyle = '#dffdf2'
  context.fillText(label, x + 5 * scale, textY + 13.5 * scale)
  context.restore()
}

function clamp(value: number) { return Math.max(0, Math.min(1, value)) }
