import { describe, expect, it, vi } from 'vitest'
import { PreviewController, type MuxerLike, type PreviewDependencies, type SocketLike } from './preview'

class FakeSocket implements SocketLike {
  binaryType: BinaryType = 'blob'
  onopen: ((event: Event) => void) | null = null
  onmessage: ((event: MessageEvent) => void) | null = null
  onclose: ((event: CloseEvent) => void) | null = null
  onerror: ((event: Event) => void) | null = null
  close() { this.onclose?.({} as CloseEvent) }
  open() { this.onopen?.({} as Event) }
  message(data: string | ArrayBuffer) { this.onmessage?.({ data } as MessageEvent) }
}

describe('PreviewController', () => {
  it('resets muxer on generation changes and feeds binary frames', () => {
    const sockets: FakeSocket[] = []
    const muxers: Array<MuxerLike & { feed: ReturnType<typeof vi.fn>; destroy: ReturnType<typeof vi.fn> }> = []
    const updates: string[] = []
    const dependencies: PreviewDependencies = {
      socketFactory: () => { const socket = new FakeSocket(); sockets.push(socket); return socket },
      muxerFactory: () => {
        const muxer = { feed: vi.fn(), destroy: vi.fn() }
        muxers.push(muxer)
        return muxer
      },
      setTimer: vi.fn(() => 1) as unknown as typeof window.setTimeout,
      clearTimer: vi.fn(), now: () => 1000, mseSupported: () => true,
      websocketUrl: () => 'ws://test/api/v1/preview/ws',
    }
    const controller = new PreviewController((state) => updates.push(state.state), dependencies)
    controller.connect({} as HTMLVideoElement)
    sockets[0].open()
    sockets[0].message(JSON.stringify({ type: 'stream', stream: {
      generation: 'g1', codec: 'h264', format: 'annexb', width: 1280, height: 720, fps: 25,
    }}))
    sockets[0].message(new Uint8Array([0, 0, 0, 1, 0x65]).buffer)
    expect(muxers[0].feed).toHaveBeenCalledOnce()
    expect(updates.at(-1)).toBe('live')
    sockets[0].message(JSON.stringify({ type: 'reset' }))
    expect(muxers[0].destroy).toHaveBeenCalledOnce()
    controller.disconnect()
  })

  it('does not reconnect after manual disconnect', () => {
    const socket = new FakeSocket()
    const setTimer = vi.fn(() => 1) as unknown as typeof window.setTimeout
    const dependencies: PreviewDependencies = {
      socketFactory: () => socket,
      muxerFactory: () => ({ feed() {}, destroy() {} }),
      setTimer, clearTimer: vi.fn(), now: () => 0, mseSupported: () => true,
      websocketUrl: () => 'ws://test/api/v1/preview/ws',
    }
    const controller = new PreviewController(() => {}, dependencies)
    controller.connect({} as HTMLVideoElement)
    controller.disconnect()
    expect(setTimer).not.toHaveBeenCalled()
  })
})
