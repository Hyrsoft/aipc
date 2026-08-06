import { describe, expect, it } from 'vitest'
import {
  currentBitrate, currentVideoFps, eventLevel, eventName, eventSummary,
  formatDuration, isEventViewportAtBottom, matchesEventFilter, workerUptimeSeconds,
} from './telemetry'
import type { ServerEvent } from './types'

function event(kind: string, payload: unknown): ServerEvent {
  return { kind, timestamp_ms: 1, payload }
}

describe('metric compatibility mapping', () => {
  it('prefers interval rates and falls back to legacy averages', () => {
    expect(currentVideoFps({ packets: 1, bytes: 1, keyframes: 0, last_pts: 0, timeouts: 0, errors: 0, fps: 29.8, average_fps: 25 })).toBe(29.8)
    expect(currentVideoFps({ packets: 1, bytes: 1, keyframes: 0, last_pts: 0, timeouts: 0, errors: 0, average_fps: 25 })).toBe(25)
    expect(currentBitrate({ packets: 1, bytes: 1, keyframes: 0, last_pts: 0, timeouts: 0, errors: 0, bitrate_kbps: 2048, average_bitrate_kbps: 1900 })).toBe(2048)
    expect(currentBitrate({ packets: 1, bytes: 1, keyframes: 0, last_pts: 0, timeouts: 0, errors: 0, average_bitrate_kbps: 1900 })).toBe(1900)
    expect(currentVideoFps(undefined)).toBeNull()
  })
})

describe('event presentation', () => {
  it('classifies structured and textual severity', () => {
    expect(eventLevel(event('worker_event', { event: 'StreamReady' }))).toBe('info')
    expect(eventLevel(event('worker_event', { event: 'Warning' }))).toBe('warn')
    expect(eventLevel(event('worker_event', { event: 'StreamStalled' }))).toBe('warn')
    expect(eventLevel(event('worker_event', { event: 'FatalError' }))).toBe('error')
    expect(eventLevel(event('status', { state: 'backoff' }))).toBe('warn')
    expect(eventLevel(event('status', { state: 'failed' }))).toBe('error')
    expect(eventLevel(event('lagged', { skipped: 3 }))).toBe('warn')
    expect(eventLevel(event('log', { line: 'ERROR encoder failed' }))).toBe('error')
  })

  it('filters, names and summarizes events', () => {
    const warning = event('worker_event', { event: 'Warning', media: 'video', reason: 'timeout' })
    expect(matchesEventFilter(warning, 'warn')).toBe(true)
    expect(matchesEventFilter(warning, 'error')).toBe(false)
    expect(eventName(warning)).toBe('Warning')
    expect(eventSummary(warning)).toBe('video · timeout')

    const metrics = event('worker_event', {
      event: 'Metrics',
      video: { packets: 1, bytes: 1, keyframes: 0, last_pts: 0, timeouts: 0, errors: 0, fps: 30, bitrate_kbps: 2048 },
    })
    expect(eventSummary(metrics)).toContain('30.0 fps')
  })

  it('detects whether automatic following should remain active', () => {
    expect(isEventViewportAtBottom(1000, 600, 380)).toBe(true)
    expect(isEventViewportAtBottom(1000, 500, 380)).toBe(false)
  })
})

describe('worker runtime presentation', () => {
  it('uses board elapsed time plus a local ticking delta without comparing wall clocks', () => {
    const status = {
      state: 'running', pid: 42, generation: 'g1', stage: 'ready', started_at_ms: 5_000,
      updated_at_ms: 10_000, restart_count: 2, video_ready: true, audio_ready: true,
      last_error: null, metrics: null,
    }
    expect(workerUptimeSeconds(status, 1_000_000, 1_002_500)).toBe(7)
    expect(workerUptimeSeconds({ ...status, pid: null }, 1_000_000, 1_002_500)).toBe(0)
  })

  it('formats runtime as a clock and includes days when needed', () => {
    expect(formatDuration(65)).toBe('00:01:05')
    expect(formatDuration(90061)).toBe('1d 01:01:01')
  })
})
