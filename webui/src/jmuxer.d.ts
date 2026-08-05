declare module 'jmuxer' {
  export interface JMuxerOptions {
    node: HTMLVideoElement
    mode: 'video'
    flushingTime?: number
    maxDelay?: number
    clearBuffer?: boolean
    fps?: number
    debug?: boolean
    onError?: (error: unknown) => void
    onMissingVideoFrames?: (count: number) => void
  }

  export default class JMuxer {
    constructor(options: JMuxerOptions)
    feed(data: { video: Uint8Array; duration?: number }): void
    destroy(): void
  }
}
