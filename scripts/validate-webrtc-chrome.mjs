#!/usr/bin/env node

import { spawn } from 'node:child_process'
import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

const baseUrl = (process.argv[2] || 'http://192.168.8.106:8080').replace(/\/$/, '')
const chrome = process.env.CHROME_BIN || '/usr/bin/google-chrome-stable'
const debugPort = Number(process.env.AIPC_CHROME_DEBUG_PORT || 19223)
const timeoutMs = Number(process.env.AIPC_WEBRTC_TIMEOUT_MS || 20_000)
const forcedProfile = process.env.AIPC_WEBRTC_FORCE_PROFILE || ''
const dumpSdp = process.env.AIPC_WEBRTC_DUMP_SDP === '1'
const profile = mkdtempSync(join(tmpdir(), 'aipc-webrtc-chrome.'))
const child = spawn(chrome, [
  '--headless=new',
  '--no-sandbox',
  '--disable-gpu',
  '--autoplay-policy=no-user-gesture-required',
  `--remote-debugging-port=${debugPort}`,
  `--user-data-dir=${profile}`,
  `${baseUrl}/api/v1/webrtc/status`,
], { stdio: ['ignore', 'ignore', 'pipe'] })

let chromeErrors = ''
child.stderr.on('data', (chunk) => { chromeErrors += chunk.toString() })

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms))
}

async function findPage() {
  const deadline = Date.now() + 10_000
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${debugPort}/json/list`)
      const pages = await response.json()
      const page = pages.find((item) => item.type === 'page')
      if (page?.webSocketDebuggerUrl) return page
    } catch {
      // Chrome may still be starting.
    }
    await delay(100)
  }
  throw new Error(`Chrome DevTools did not start:\n${chromeErrors}`)
}

class CdpClient {
  constructor(url) {
    this.socket = new WebSocket(url)
    this.sequence = 0
    this.pending = new Map()
  }

  async open() {
    if (this.socket.readyState === WebSocket.OPEN) return
    await new Promise((resolve, reject) => {
      this.socket.addEventListener('open', resolve, { once: true })
      this.socket.addEventListener('error', reject, { once: true })
    })
  }

  call(method, params = {}) {
    const id = ++this.sequence
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject })
      this.socket.send(JSON.stringify({ id, method, params }))
    })
  }

  start() {
    this.socket.addEventListener('message', (event) => {
      const message = JSON.parse(event.data)
      if (!message.id) return
      const pending = this.pending.get(message.id)
      if (!pending) return
      this.pending.delete(message.id)
      if (message.error) pending.reject(new Error(message.error.message))
      else pending.resolve(message.result)
    })
  }

  close() {
    this.socket.close()
  }
}

const browserProbe = `
(async () => {
  const baseUrl = ${JSON.stringify(baseUrl)};
  const timeoutMs = ${timeoutMs};
  const forcedProfile = ${JSON.stringify(forcedProfile)};
  const dumpSdp = ${dumpSdp};
  const pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle' });
  let sessionId = null;
  const tracks = { video: 0, audio: 0 };
  pc.addTransceiver('video', { direction: 'recvonly' });
  pc.addTransceiver('audio', { direction: 'recvonly' });
  pc.ontrack = (event) => { tracks[event.track.kind] += 1; };

  function waitForIceGathering() {
    if (pc.iceGatheringState === 'complete') return Promise.resolve();
    return new Promise((resolve) => {
      const timer = setTimeout(done, 3000);
      function done() {
        clearTimeout(timer);
        pc.removeEventListener('icegatheringstatechange', changed);
        resolve();
      }
      function changed() {
        if (pc.iceGatheringState === 'complete') done();
      }
      pc.addEventListener('icegatheringstatechange', changed);
    });
  }

  async function mediaStats() {
    const result = { videoBytes: 0, videoFrames: 0, audioBytes: 0, audioPackets: 0, packetsLost: 0 };
    const reports = await pc.getStats();
    reports.forEach((report) => {
      if (report.type !== 'inbound-rtp') return;
      const kind = report.kind || report.mediaType;
      result.packetsLost += Number(report.packetsLost || 0);
      if (kind === 'video') {
        result.videoBytes += Number(report.bytesReceived || 0);
        result.videoFrames += Number(report.framesDecoded || 0);
      } else if (kind === 'audio') {
        result.audioBytes += Number(report.bytesReceived || 0);
        result.audioPackets += Number(report.packetsReceived || 0);
      }
    });
    return result;
  }

  try {
    const offer = await pc.createOffer();
    const offerSdp = forcedProfile
      ? offer.sdp.replace(/profile-level-id=f4001f/gi, 'profile-level-id=' + forcedProfile)
      : offer.sdp;
    await pc.setLocalDescription({ type: offer.type, sdp: offerSdp });
    await waitForIceGathering();
    const response = await fetch(baseUrl + '/api/v1/webrtc/sessions', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(pc.localDescription),
    });
    const answer = await response.json().catch(() => ({}));
    if (!response.ok) {
      return {
        ok: false,
        error: JSON.stringify(answer),
        offeredH264Profiles: [...new Set(
          [...(pc.localDescription?.sdp || '').matchAll(/profile-level-id=([0-9a-f]{6})/gi)]
            .map((match) => match[1].toLowerCase())
        )],
        offerSdp: dumpSdp ? pc.localDescription?.sdp || '' : undefined,
      };
    }
    sessionId = answer.id;
    await pc.setRemoteDescription({ type: answer.type, sdp: answer.sdp });

    const deadline = performance.now() + timeoutMs;
    let stats = await mediaStats();
    while (performance.now() < deadline) {
      if (pc.connectionState === 'failed' || pc.connectionState === 'closed') break;
      stats = await mediaStats();
      if (pc.connectionState === 'connected' && stats.videoBytes > 0 && stats.audioPackets > 0) break;
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
    return {
      ok: pc.connectionState === 'connected' && stats.videoBytes > 0 && stats.audioPackets > 0,
      connectionState: pc.connectionState,
      iceConnectionState: pc.iceConnectionState,
      tracks,
      stats,
      sessionId,
      answerHasH264: /H264\\/90000/i.test(answer.sdp || ''),
      answerHasPcma: /PCMA\\/8000/i.test(answer.sdp || ''),
    };
  } finally {
    if (sessionId) {
      await fetch(baseUrl + '/api/v1/webrtc/sessions/' + encodeURIComponent(sessionId), { method: 'DELETE' }).catch(() => {});
    }
    pc.close();
  }
})()
`

let cdp
try {
  const page = await findPage()
  cdp = new CdpClient(page.webSocketDebuggerUrl)
  await cdp.open()
  cdp.start()
  const pageDeadline = Date.now() + 10_000
  while (Date.now() < pageDeadline) {
    try {
      const state = await cdp.call('Runtime.evaluate', {
        expression: '({ href: location.href, ready: document.readyState })',
        returnByValue: true,
      })
      if (state.result?.value?.href?.startsWith(baseUrl) && state.result.value.ready === 'complete') break
    } catch {
      // The first execution context is replaced while Chrome navigates.
    }
    await delay(100)
  }
  const evaluation = await cdp.call('Runtime.evaluate', {
    expression: browserProbe,
    awaitPromise: true,
    returnByValue: true,
  })
  if (evaluation.exceptionDetails) {
    throw new Error(evaluation.exceptionDetails.exception?.description || evaluation.exceptionDetails.text)
  }
  const result = evaluation.result.value
  console.log(JSON.stringify(result, null, 2))
  if (!result?.ok) process.exitCode = 1
} finally {
  cdp?.close()
  child.kill('SIGTERM')
  await Promise.race([new Promise((resolve) => child.once('exit', resolve)), delay(2000)])
  if (!child.killed) child.kill('SIGKILL')
  rmSync(profile, { recursive: true, force: true })
}
