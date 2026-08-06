#!/usr/bin/env node

import { createWriteStream } from 'node:fs'

const [url, outputPath, durationText = '5'] = process.argv.slice(2)
if (!url || !outputPath) {
  console.error('usage: capture-preview.mjs <ws-url> <output.h264> [duration-sec]')
  process.exit(2)
}

const durationMs = Number(durationText) * 1000
const started = performance.now()
const output = createWriteStream(outputPath)
const socket = new WebSocket(url)
socket.binaryType = 'arraybuffer'

let stream = null
let frames = 0
let bytes = 0
let firstFrameMs = null
let audioFrames = 0
let audioBytes = 0
let stopping = false

const deadline = setTimeout(() => finish(new Error('preview first-frame timeout')), 10000)

socket.onmessage = (event) => {
  if (typeof event.data === 'string') {
    const message = JSON.parse(event.data)
    if (message.type === 'stream') stream = message.stream
    return
  }
  const data = Buffer.from(event.data)
  if (data.length === 0) return
  if (data.length >= 28 && data.subarray(0, 4).toString('ascii') === 'AIPA') {
    audioFrames += 1
    audioBytes += data.readUInt32BE(8)
    return
  }
  if (firstFrameMs === null) {
    firstFrameMs = Math.round(performance.now() - started)
    clearTimeout(deadline)
    setTimeout(() => finish(), durationMs)
  }
  frames += 1
  bytes += data.length
  output.write(data)
}

socket.onerror = () => finish(new Error('preview WebSocket error'))
socket.onclose = () => {
  if (!stopping) finish(new Error('preview WebSocket closed early'))
}

function finish(error = null) {
  if (stopping) return
  stopping = true
  clearTimeout(deadline)
  socket.close()
  output.end(() => {
    if (error || !stream || frames === 0) {
      console.error(error?.message || 'preview produced no stream metadata or frames')
      process.exit(1)
    }
    console.log(JSON.stringify({ stream, frames, bytes, audio_frames: audioFrames, audio_bytes: audioBytes, first_frame_ms: firstFrameMs }))
  })
}
