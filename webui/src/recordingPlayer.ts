export const PLAYBACK_RATES = [0.5, 0.75, 1, 1.25, 1.5, 2] as const

export function clampPlaybackTime(current: number, delta: number, duration: number) {
  return Math.max(0, Math.min(Number.isFinite(duration) ? duration : 0, current + delta))
}

export function formatPlaybackTime(seconds: number) {
  if (!Number.isFinite(seconds)) return '00:00'
  const total = Math.max(0, Math.floor(seconds))
  return `${String(Math.floor(total / 60)).padStart(2, '0')}:${String(total % 60).padStart(2, '0')}`
}
