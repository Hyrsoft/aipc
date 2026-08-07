<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { api, connectEvents, reduceServerEvent, type LiveState } from './api'
import type { PersistentState, WorkerConfig } from './types'
import { initialPreviewSnapshot } from './preview'
import { AdaptivePreviewController } from './webrtcPreview'
import { AiOverlayController } from './aiOverlay'
import AiView from './AiView.vue'
import RecordingsView from './RecordingsView.vue'
import {
  currentBitrate, currentVideoFps, eventLevel, eventName, eventSummary, matchesEventFilter,
  formatDuration, isEventViewportAtBottom, workerUptimeSeconds,
  type EventFilter,
} from './telemetry'

const live = reactive<LiveState>({ status: null, events: [], logs: [] })
const configs = ref<PersistentState | null>(null)
const form = ref<WorkerConfig | null>(null)
const connected = ref(false)
const busy = ref(false)
const notice = ref('')
const error = ref('')
const trackingGeneration = ref<string | null>(null)
let disconnect: (() => void) | undefined
const previewVideo = ref<HTMLVideoElement | null>(null)
const aiCanvas = ref<HTMLCanvasElement | null>(null)
const aiOverlayEnabled = ref(window.localStorage.getItem('aipc-ai-overlay') !== 'off')
const preview = reactive(initialPreviewSnapshot())
const previewManuallyStopped = ref(false)
const eventViewport = ref<HTMLDivElement | null>(null)
const eventFilter = ref<EventFilter>('all')
const eventFilters: EventFilter[] = ['all', 'info', 'warn', 'error']
const eventFollow = ref(true)
const activeView = ref<'overview' | 'recordings' | 'ai' | 'settings' | 'diagnostics'>('overview')
const clockMs = ref(Date.now())
const statusReceivedAtMs = ref(Date.now())
let previewController: AdaptivePreviewController | undefined
let aiOverlay: AiOverlayController | undefined
let clockTimer: number | undefined

const status = computed(() => live.status)
const metrics = computed(() => status.value?.metrics)
const videoMetrics = computed(() => metrics.value?.video)
const audioMetrics = computed(() => metrics.value?.audio)
const videoFps = computed(() => currentVideoFps(videoMetrics.value))
const videoBitrate = computed(() => currentBitrate(videoMetrics.value))
const audioBitrate = computed(() => currentBitrate(audioMetrics.value))
const activeAudioEnabled = computed(() =>
  configs.value?.active?.audio.enabled ?? configs.value?.desired?.audio.enabled ?? form.value?.audio.enabled ?? true,
)
const videoMetricState = computed(() => videoFps.value === null ? '等待指标' : '')
const audioMetricState = computed(() => !activeAudioEnabled.value ? '已禁用' : audioBitrate.value === null ? '等待指标' : '')
const filteredEvents = computed(() => live.events.filter((event) => matchesEventFilter(event, eventFilter.value)))
const uptime = computed(() => formatDuration(workerUptimeSeconds(status.value, statusReceivedAtMs.value, clockMs.value)))
const startedAt = computed(() => status.value?.pid && status.value.started_at_ms
  ? new Date(status.value.started_at_ms).toLocaleString()
  : '—')
const configDiff = computed(() => {
  if (!configs.value) return []
  const rows: Array<{ name: string; value: WorkerConfig | null }> = [
    { name: 'desired', value: configs.value.desired }, { name: 'active', value: configs.value.active },
    { name: 'pending', value: configs.value.pending }, { name: 'last-good', value: configs.value.last_good },
  ]
  return rows
})

function assignLive(next: LiveState) {
  live.status = next.status
  live.events = next.events
  live.logs = next.logs
  if (trackingGeneration.value && next.status?.generation === trackingGeneration.value && ['running', 'failed'].includes(next.status.state)) {
    notice.value = next.status.state === 'running' ? `Generation ${trackingGeneration.value.slice(0, 8)} 已运行` : '新配置启动失败'
    trackingGeneration.value = null
    refreshConfig()
  }
}

async function refreshConfig() {
  configs.value = await api.config()
  if (!form.value) form.value = structuredClone(configs.value.desired || configs.value.last_good)
}

async function load() {
  try {
    const [nextStatus, nextConfig, nextLogs] = await Promise.all([api.status(), api.config(), api.logs(100)])
    live.status = nextStatus; live.logs = nextLogs; configs.value = nextConfig
    form.value = structuredClone(nextConfig.desired || nextConfig.last_good)
  } catch (cause) { error.value = String(cause) }
}

async function control(action: 'start' | 'stop' | 'restart') {
  busy.value = true; error.value = ''; notice.value = ''
  try {
    const result = await api.control(action)
    trackingGeneration.value = result.generation
    notice.value = `${action} 请求已接受`
  } catch (cause) { error.value = String(cause) } finally { busy.value = false }
}

async function applyConfig() {
  if (!form.value || !confirm('应用配置会冷重启 media worker，是否继续？')) return
  busy.value = true; error.value = ''; notice.value = ''
  try {
    const result = await api.apply(form.value)
    trackingGeneration.value = result.generation
    notice.value = `正在切换到 generation ${result.generation.slice(0, 8)}`
    await refreshConfig()
  } catch (cause) { error.value = String(cause) } finally { busy.value = false }
}

function resetForm() { if (configs.value?.desired) form.value = structuredClone(configs.value.desired) }
function fmt(value: unknown, digits = 1) { return typeof value === 'number' ? value.toFixed(digits) : '—' }
function short(value: string | null | undefined) { return value ? value.slice(0, 8) : '—' }
function bytes(value: number) { return value > 1024 * 1024 ? `${(value / 1024 / 1024).toFixed(1)} MiB` : `${(value / 1024).toFixed(1)} KiB` }
function clearEvents() { live.events = [] }
function resumeEventFollow() {
  eventFollow.value = true
  scrollEventsToBottom()
}
function handleEventScroll() {
  const viewport = eventViewport.value
  if (!viewport) return
  eventFollow.value = isEventViewportAtBottom(viewport.scrollHeight, viewport.scrollTop, viewport.clientHeight)
}
async function scrollEventsToBottom() {
  await nextTick()
  if (eventFollow.value && eventViewport.value) eventViewport.value.scrollTop = eventViewport.value.scrollHeight
}
function connectPreview() {
  previewManuallyStopped.value = false
  if (previewVideo.value) previewController?.connect(previewVideo.value)
}
function disconnectPreview() {
  previewManuallyStopped.value = true
  previewController?.disconnect()
}
function setPreviewVolume(value: number) { previewController?.setVolume(value) }
function togglePreviewMute() { previewController?.setMuted(!preview.muted) }
function toggleAiOverlay() {
  aiOverlayEnabled.value = !aiOverlayEnabled.value
  aiOverlay?.setEnabled(aiOverlayEnabled.value)
}
function attachAiOverlay() {
  aiOverlay?.destroy()
  aiOverlay = previewVideo.value && aiCanvas.value ? new AiOverlayController(previewVideo.value, aiCanvas.value) : undefined
  aiOverlay?.setEnabled(aiOverlayEnabled.value)
}

onMounted(() => {
  clockTimer = window.setInterval(() => { clockMs.value = Date.now() }, 1000)
  previewController = new AdaptivePreviewController((snapshot) => Object.assign(preview, snapshot))
  attachAiOverlay()
  load()
  disconnect = connectEvents((event) => assignLive(reduceServerEvent({ ...live }, event)), (up) => connected.value = up)
})
watch(() => [status.value?.state, status.value?.video_ready, status.value?.generation], async () => {
  if (status.value?.state === 'running' && status.value.video_ready && !previewManuallyStopped.value) {
    await nextTick()
    connectPreview()
  }
})
watch(() => status.value?.updated_at_ms, () => { statusReceivedAtMs.value = Date.now() })
watch(() => [live.events.at(-1), eventFilter.value], () => scrollEventsToBottom(), { flush: 'post' })
watch(activeView, async (view) => {
  if (view !== 'overview') {
    previewController?.disconnect()
    aiOverlay?.destroy()
    aiOverlay = undefined
    return
  }
  await nextTick()
  attachAiOverlay()
  if (status.value?.state === 'running' && status.value.video_ready && !previewManuallyStopped.value) {
    await nextTick()
    connectPreview()
  }
})
onBeforeUnmount(() => { disconnect?.(); previewController?.destroy(); aiOverlay?.destroy(); if (clockTimer) window.clearInterval(clockTimer) })
</script>

<template>
  <div class="app-shell">
    <aside class="sidebar">
      <div class="brand"><span class="brand-mark">AI</span><div><p class="eyebrow">RV1106 CONTROL</p><h1>Media Console</h1></div></div>
      <nav class="nav-tabs" aria-label="主导航">
        <button :class="activeView === 'overview' && 'active'" @click="activeView = 'overview'"><span>01</span>运行概览</button>
        <button :class="activeView === 'recordings' && 'active'" @click="activeView = 'recordings'"><span>02</span>录像管理</button>
        <button :class="activeView === 'ai' && 'active'" @click="activeView = 'ai'"><span>03</span>AI 与 Lua</button>
        <button :class="activeView === 'settings' && 'active'" @click="activeView = 'settings'"><span>04</span>媒体配置</button>
        <button :class="activeView === 'diagnostics' && 'active'" @click="activeView = 'diagnostics'"><span>05</span>日志诊断<i v-if="live.events.some(event => eventLevel(event) === 'error')"></i></button>
      </nav>
      <div class="sidebar-meta">
        <div class="connection"><span :class="['dot', connected && 'online']"></span><div><b>{{ connected ? '服务已连接' : '正在重连' }}</b><small>SSE EVENT STREAM</small></div></div>
        <p>未启用身份认证<br>仅限可信局域网使用</p>
      </div>
    </aside>

    <main>
      <header class="topbar">
        <div><p class="eyebrow">{{ activeView === 'overview' ? 'OVERVIEW' : activeView === 'recordings' ? 'RECORDINGS' : activeView === 'ai' ? 'AI WORKER' : activeView === 'settings' ? 'CONFIGURATION' : 'DIAGNOSTICS' }}</p><h2>{{ activeView === 'overview' ? '运行概览' : activeView === 'recordings' ? '录像管理' : activeView === 'ai' ? 'AI 与 Lua 管理' : activeView === 'settings' ? '媒体配置' : '日志诊断' }}</h2></div>
        <div class="header-status"><span :class="['status-pill', status?.state]">{{ status?.state || 'offline' }}</span><span>运行 {{ uptime }}</span></div>
      </header>

      <div v-if="notice" class="notice">{{ notice }}</div>
      <div v-if="error" class="error">{{ error }}</div>

      <template v-if="activeView === 'overview'">
        <section class="hero panel">
          <div class="state-block"><span class="label">WORKER STATE</span><div :class="['state', status?.state]">{{ status?.state || 'offline' }}</div><p>{{ status?.stage || '等待 daemon 状态' }}</p></div>
          <div class="hero-stats">
            <div><span>PID</span><strong>{{ status?.pid || '—' }}</strong></div>
            <div><span>GENERATION</span><strong>{{ short(status?.generation) }}</strong></div>
            <div><span>STARTED</span><strong>{{ startedAt }}</strong></div>
            <div><span>RESTARTS</span><strong>{{ status?.restart_count ?? 0 }}</strong></div>
          </div>
          <div class="controls"><button :disabled="busy" @click="control('start')">启动</button><button class="secondary danger" :disabled="busy" @click="control('stop')">停止</button><button class="accent" :disabled="busy" @click="control('restart')">重启 Worker</button></div>
        </section>

        <section class="overview-grid">
          <article class="panel preview-panel">
            <div class="section-head preview-head"><div><span class="label">LIVE PREVIEW</span><h3>实时画面</h3></div><div class="preview-actions"><button class="secondary compact" :class="aiOverlayEnabled && 'selected'" @click="toggleAiOverlay">AI 框 {{ aiOverlayEnabled ? '开' : '关' }}</button><span :class="['preview-state', preview.state]">{{ preview.state }}</span><button v-if="preview.state === 'disconnected' || preview.state === 'error'" class="accent compact" @click="connectPreview">连接</button><button v-else class="secondary compact" @click="disconnectPreview">断开</button></div></div>
            <div class="preview-stage"><video ref="previewVideo" autoplay :muted="preview.muted" playsinline></video><canvas ref="aiCanvas" class="ai-overlay-canvas"></canvas><div v-if="preview.state !== 'live'" class="preview-overlay"><span class="loader"></span><strong>{{ preview.state === 'unsupported' ? '浏览器不支持实时预览' : preview.state === 'error' ? '预览连接异常' : '等待视频流' }}</strong><span>{{ preview.error || '优先连接 WebRTC，失败时回退 WebSocket' }}</span></div></div>
            <div class="preview-stats"><span>分辨率 <b>{{ preview.stream ? `${preview.stream.width} × ${preview.stream.height}` : '—' }}</b></span><span>接收帧率 <b>{{ fmt(preview.receivedFps) }} FPS</b></span><span>实时码率 <b>{{ fmt(preview.bitrateKbps) }} Kbps</b></span><span>丢帧 / 重连 <b>{{ preview.droppedFrames }} / {{ preview.reconnects }}</b></span></div>
            <div class="audio-preview-controls"><span :class="['preview-state', preview.audioState]">音频 {{ preview.audioState }}</span><span>{{ fmt(preview.audioBitrateKbps) }} Kbps · {{ preview.audioPackets }} 包</span><button class="secondary compact" @click="togglePreviewMute">{{ preview.muted ? '取消静音' : '静音' }}</button><input aria-label="预览音量" type="range" min="0" max="1" step="0.05" :value="preview.volume" @input="setPreviewVolume(Number(($event.target as HTMLInputElement).value))"><span v-if="preview.audioError" class="audio-error">{{ preview.audioError }}</span></div>
          </article>

          <div class="metric-column">
            <article class="panel metric-card"><div class="card-title"><span>VIDEO · H264</span><i :class="status?.video_ready && 'ready'"></i></div><div class="metric"><strong :class="videoFps === null && 'placeholder'">{{ videoFps === null ? videoMetricState : fmt(videoFps) }}</strong><span>{{ videoFps === null ? '' : 'FPS' }}</span></div><div class="mini"><span>码率<b>{{ videoBitrate === null ? videoMetricState : `${fmt(videoBitrate)} Kbps` }}</b></span><span>数据包<b>{{ videoMetrics?.packets ?? '—' }}</b></span><span>关键帧<b>{{ videoMetrics?.keyframes ?? '—' }}</b></span><span>错误 / 超时<b>{{ videoMetrics?.errors ?? '—' }} / {{ videoMetrics?.timeouts ?? '—' }}</b></span></div></article>
            <article class="panel metric-card"><div class="card-title"><span>AUDIO · G711A</span><i :class="status?.audio_ready && 'ready'"></i></div><div class="metric"><strong :class="audioBitrate === null && 'placeholder'">{{ audioBitrate === null ? audioMetricState : fmt(audioBitrate) }}</strong><span>{{ audioBitrate === null ? '' : 'Kbps' }}</span></div><div class="mini"><span>数据包<b>{{ audioMetrics?.packets ?? audioMetricState }}</b></span><span>传输字节<b>{{ audioMetrics?.bytes ?? audioMetricState }}</b></span><span>错误 / 超时<b>{{ audioMetrics?.errors ?? '—' }} / {{ audioMetrics?.timeouts ?? '—' }}</b></span></div></article>
          </div>
        </section>
      </template>

      <RecordingsView v-else-if="activeView === 'recordings'" />

      <AiView v-else-if="activeView === 'ai'" />

      <template v-else-if="activeView === 'settings'">
        <section class="panel config" v-if="form">
          <div class="section-head"><div><span class="label">STREAM SETTINGS</span><h3>常用参数</h3><p>应用配置将预检并冷重启 Worker，失败时自动回滚。</p></div><div class="section-actions"><button class="secondary" @click="resetForm">恢复当前值</button><button class="accent" :disabled="busy" @click="applyConfig">应用配置</button></div></div>
          <div class="form-section"><h4>视频编码</h4><div class="form-grid primary-form"><label>分辨率<select v-model="form.video.width" @change="form.video.height = form.video.width === 1280 ? 720 : 1080"><option :value="1920">1920 × 1080</option><option :value="1280">1280 × 720</option></select></label><label>帧率 (FPS)<input type="number" min="1" max="60" v-model.number="form.video.fps"></label><label>码率 (Kbps)<input type="number" min="64" max="50000" v-model.number="form.video.bitrate_kbps"></label><label>关键帧间隔 (GOP)<input type="number" min="1" max="300" v-model.number="form.video.gop"></label></div></div>
          <div class="form-section audio-row"><div><h4>音频编码</h4><p>G711A v1 固定 8 kHz、单声道、16-bit、64 kbps；设备侧可配置采样率与 1/2 通道。</p></div><label class="toggle"><input type="checkbox" v-model="form.audio.enabled"><span></span>{{ form.audio.enabled ? '已启用' : '已关闭' }}</label></div>
          <div class="form-grid primary-form audio-config" :class="!form.audio.enabled && 'disabled'"><label>声卡名称<input v-model="form.audio.card_name" :disabled="!form.audio.enabled"></label><label>AI device<input type="number" min="0" v-model.number="form.audio.device_id" :disabled="!form.audio.enabled"></label><label>AI channel<input type="number" min="0" v-model.number="form.audio.channel_id" :disabled="!form.audio.enabled"></label><label>AENC channel<input type="number" min="0" v-model.number="form.audio.aenc_channel_id" :disabled="!form.audio.enabled"></label><label>设备采样率<input type="number" min="8000" max="48000" v-model.number="form.audio.device_sample_rate" :disabled="!form.audio.enabled"></label><label>编码采样率<input type="number" value="8000" v-model.number="form.audio.sample_rate" :disabled="!form.audio.enabled"></label><label>设备通道数<select v-model.number="form.audio.device_channels" :disabled="!form.audio.enabled"><option :value="1">1</option><option :value="2">2</option></select></label><label>编码通道数<input type="number" value="1" v-model.number="form.audio.channels" :disabled="!form.audio.enabled"></label><label>输入位宽<input type="number" value="16" v-model.number="form.audio.bit_width" :disabled="!form.audio.enabled"></label><label>每帧采样数<input type="number" min="80" max="2048" v-model.number="form.audio.frame_samples" :disabled="!form.audio.enabled"></label><label>G711A 码率<input type="number" value="64000" v-model.number="form.audio.bitrate" :disabled="!form.audio.enabled"></label><label>Buffer 数量<input type="number" min="2" max="16" v-model.number="form.audio.buffer_count" :disabled="!form.audio.enabled"></label></div>
          <details class="advanced-details"><summary><div><b>高级硬件与运行时参数</b><small>诊断输出不是业务数据通道，正式音频由 AIPA IPC 提供</small></div><span>展开</span></summary><div class="form-grid advanced"><label>IQ 目录<input v-model="form.isp.iq_dir"></label><label>VI device<input type="number" v-model.number="form.vi.device_id"></label><label>VI pipe<input type="number" v-model.number="form.vi.pipe_id"></label><label>VI channel<input type="number" v-model.number="form.vi.channel_id"></label><label>VI buffers<input type="number" v-model.number="form.vi.buffer_count"></label><label>VPSS group<input type="number" v-model.number="form.vpss.group_id"></label><label>VPSS channel<input type="number" v-model.number="form.vpss.channel_id"></label><label>VENC channel<input type="number" v-model.number="form.video.venc_channel_id"></label><label>VENC buffers<input type="number" v-model.number="form.video.stream_buffer_count"></label><label>Warning timeout<input type="number" v-model.number="form.runtime.warning_timeout_count"></label><label>Stalled timeout<input type="number" v-model.number="form.runtime.stalled_timeout_count"></label><label>Fatal timeout<input type="number" v-model.number="form.runtime.fatal_timeout_count"></label><label>Metrics (ms)<input type="number" v-model.number="form.runtime.metrics_interval_ms"></label><label class="wide">视频诊断输出<input v-model="form.video.output_path"></label><label class="wide">音频诊断输出<input v-model="form.audio.output_path"></label></div></details>
        </section>
        <section class="panel config-versions"><div class="section-head"><div><span class="label">CONFIG HISTORY</span><h3>配置版本</h3><p>用于核对当前生效、待应用与最近可用配置。</p></div></div><div class="config-stack"><details v-for="row in configDiff" :key="row.name"><summary><b>{{ row.name }}</b><span>{{ short(row.value?.runtime.generation) }}</span></summary><pre>{{ JSON.stringify(row.value, null, 2) }}</pre></details></div><p v-if="configs?.last_error" class="rollback">最近回滚 / 错误：{{ configs.last_error }}</p></section>
      </template>

      <template v-else>
        <section class="diagnostic-grid">
          <article class="panel terminal"><div class="section-head"><div><span class="label">WORKER STDERR</span><h3>运行日志</h3></div><span class="count">{{ live.logs.length }} / 200</span></div><div class="log-lines"><p v-for="(line, i) in live.logs.slice(-100)" :key="i"><time>{{ new Date(line.timestamp_ms).toLocaleTimeString() }}</time> {{ line.line }}</p><p v-if="!live.logs.length" class="empty-state">暂无 stderr 输出</p></div></article>
          <article class="panel events"><div class="section-head event-head"><div><span class="label">EVENT STREAM</span><h3>结构化事件</h3><p>{{ filteredEvents.length }} 条匹配事件</p></div><div class="event-tools"><div class="event-filters"><button v-for="filter in eventFilters" :key="filter" class="secondary compact" :class="eventFilter === filter && 'selected'" @click="eventFilter = filter">{{ filter }}</button></div><button v-if="!eventFollow" class="secondary compact" @click="resumeEventFollow">继续跟随</button><button class="secondary compact" @click="clearEvents">清空</button></div></div><div ref="eventViewport" class="event-lines" @scroll="handleEventScroll"><details v-for="(event, index) in filteredEvents" :key="`${event.timestamp_ms}-${event.kind}-${index}`" :class="['event-row', `event-${eventLevel(event)}`]"><summary><time>{{ new Date(event.timestamp_ms).toLocaleTimeString() }}</time><span class="event-level">{{ eventLevel(event) }}</span><b>{{ eventName(event) }}</b><span class="event-summary">{{ eventSummary(event) }}</span></summary><pre>{{ JSON.stringify(event.payload, null, 2) }}</pre></details><p v-if="!filteredEvents.length" class="empty-state">当前过滤条件下暂无事件</p></div></article>
        </section>
      </template>
    </main>
  </div>
</template>
