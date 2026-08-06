import { describe, expect, it, vi } from 'vitest'
import { AdaptivePreviewController, type PreviewControl, type WebRtcDependencies } from './webrtcPreview'

describe('AdaptivePreviewController', () => {
  it('starts the websocket fallback only once after WebRTC failure', async () => {
    const fallback: PreviewControl = {
      connect: vi.fn(),
      disconnect: vi.fn(),
      destroy: vi.fn(),
      setVolume: vi.fn(),
      setMuted: vi.fn(),
    }
    const dependencies: WebRtcDependencies = {
      peerFactory: vi.fn(),
      mediaStreamFactory: vi.fn(),
      fetchStatus: vi.fn().mockRejectedValue(new Error('disabled')),
      createSession: vi.fn(),
      deleteSession: vi.fn(),
      setTimer: setTimeout as unknown as typeof window.setTimeout,
      clearTimer: clearTimeout as unknown as typeof window.clearTimeout,
      now: () => 0,
    }
    const controller = new AdaptivePreviewController(
      () => undefined,
      dependencies,
      () => fallback,
    )
    const video = { muted: false, volume: 1, srcObject: null } as unknown as HTMLVideoElement

    controller.connect(video)
    await Promise.resolve()
    await Promise.resolve()
    controller.connect(video)

    expect(fallback.connect).toHaveBeenCalledTimes(1)
    controller.disconnect()
    expect(fallback.disconnect).toHaveBeenCalled()
  })

  it('does not start fallback after a manual disconnect', async () => {
    let rejectStatus: ((reason: Error) => void) | undefined
    const status = new Promise<never>((_resolve, reject) => { rejectStatus = reject })
    const fallback: PreviewControl = {
      connect: vi.fn(),
      disconnect: vi.fn(),
      destroy: vi.fn(),
      setVolume: vi.fn(),
      setMuted: vi.fn(),
    }
    const dependencies: WebRtcDependencies = {
      peerFactory: vi.fn(),
      mediaStreamFactory: vi.fn(),
      fetchStatus: vi.fn(() => status),
      createSession: vi.fn(),
      deleteSession: vi.fn(),
      setTimer: setTimeout as unknown as typeof window.setTimeout,
      clearTimer: clearTimeout as unknown as typeof window.clearTimeout,
      now: () => 0,
    }
    const controller = new AdaptivePreviewController(() => undefined, dependencies, () => fallback)
    const video = { muted: false, volume: 1, srcObject: null } as unknown as HTMLVideoElement

    controller.connect(video)
    controller.disconnect()
    rejectStatus?.(new Error('late failure'))
    await Promise.resolve()
    await Promise.resolve()

    expect(fallback.connect).not.toHaveBeenCalled()
  })
})
