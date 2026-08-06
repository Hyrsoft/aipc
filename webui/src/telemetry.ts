import type { MediaMetrics, ServerEvent, VideoMetrics } from './types'

export type EventLevel = 'info' | 'warn' | 'error'
export type EventFilter = 'all' | EventLevel

export function currentVideoFps(metrics?: VideoMetrics): number | null {
  return finite(metrics?.fps) ?? finite(metrics?.average_fps)
}

export function currentBitrate(metrics?: MediaMetrics): number | null {
  return finite(metrics?.bitrate_kbps) ?? finite(metrics?.average_bitrate_kbps)
}

export function eventLevel(event: ServerEvent): EventLevel {
  const workerEvent = text(event.payload?.event)
  const state = text(event.payload?.state)
  const line = text(event.payload?.line).toLowerCase()

  if (workerEvent === 'FatalError' || state === 'failed' || /invalid jsonl|\b(fatal|error|failed)\b/.test(line)) {
    return 'error'
  }
  if (
    workerEvent === 'Warning' || workerEvent === 'StreamStalled' || event.kind === 'lagged' ||
    state === 'backoff' || /\b(warn(?:ing)?|timeout|stalled)\b/.test(line)
  ) {
    return 'warn'
  }
  return 'info'
}

export function eventName(event: ServerEvent): string {
  if (event.kind === 'worker_event' && event.payload?.event) return String(event.payload.event)
  if (event.kind === 'status' && event.payload?.state) return `status / ${event.payload.state}`
  if (event.kind === 'log' && event.payload?.stream) return `log / ${event.payload.stream}`
  return event.kind
}

export function eventSummary(event: ServerEvent): string {
  const payload = event.payload || {}
  if (event.kind === 'log') return text(payload.line) || 'worker log'
  if (event.kind === 'status') {
    const parts = [payload.stage, payload.generation ? `generation ${String(payload.generation).slice(0, 8)}` : '']
    return parts.filter(Boolean).join(' · ') || text(payload.state)
  }
  if (event.kind === 'worker_event') {
    if (payload.event === 'Metrics') {
      const video = payload.video
      const audio = payload.audio
      const parts = []
      if (video) parts.push(`video ${formatRate(currentVideoFps(video), 'fps')} · ${formatRate(currentBitrate(video), 'Kbps')}`)
      if (audio) parts.push(`audio ${formatRate(currentBitrate(audio), 'Kbps')}`)
      return parts.join(' · ') || 'metrics updated'
    }
    return [payload.media, payload.stage, payload.message || payload.reason]
      .filter(Boolean).join(' · ') || text(payload.event)
  }
  if (event.kind === 'supervisor') return [payload.action, payload.reason].filter(Boolean).join(' · ') || 'supervisor update'
  if (event.kind === 'lagged') return `${payload.skipped ?? 0} events skipped`
  return text(payload.message) || text(payload.action) || 'event received'
}

export function matchesEventFilter(event: ServerEvent, filter: EventFilter): boolean {
  return filter === 'all' || eventLevel(event) === filter
}

export function isEventViewportAtBottom(scrollHeight: number, scrollTop: number, clientHeight: number): boolean {
  return scrollHeight - scrollTop - clientHeight < 24
}

function finite(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function text(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

function formatRate(value: number | null, unit: string): string {
  return value === null ? `waiting ${unit}` : `${value.toFixed(1)} ${unit}`
}
