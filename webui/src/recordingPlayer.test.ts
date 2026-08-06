import { describe, expect, it } from 'vitest'
import { PLAYBACK_RATES, clampPlaybackTime, formatPlaybackTime } from './recordingPlayer'

describe('recording player controls', () => {
  it('clamps ten-second seeking to the media bounds', () => {
    expect(clampPlaybackTime(5, -10, 60)).toBe(0)
    expect(clampPlaybackTime(55, 10, 60)).toBe(60)
    expect(clampPlaybackTime(20, 10, 60)).toBe(30)
  })

  it('exposes the requested playback rates and formats time', () => {
    expect(PLAYBACK_RATES).toEqual([0.5, 0.75, 1, 1.25, 1.5, 2])
    expect(formatPlaybackTime(125.9)).toBe('02:05')
  })
})
