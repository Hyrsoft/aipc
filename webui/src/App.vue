<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { api, connectEvents, reduceServerEvent, type LiveState } from './api'
import type { PersistentState, WorkerConfig } from './types'
import { PreviewController, initialPreviewSnapshot } from './preview'
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
const preview = reactive(initialPreviewSnapshot())
const previewManuallyStopped = ref(false)
const eventViewport = ref<HTMLDivElement | null>(null)
const eventFilter = ref<EventFilter>('all')
const eventFilters: EventFilter[] = ['all', 'info', 'warn', 'error']
const eventFollow = ref(true)
const clockMs = ref(Date.now())
const statusReceivedAtMs = ref(Date.now())
let previewController: PreviewController | undefined
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

onMounted(() => {
  clockTimer = window.setInterval(() => { clockMs.value = Date.now() }, 1000)
  previewController = new PreviewController((snapshot) => Object.assign(preview, snapshot))
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
onBeforeUnmount(() => { disconnect?.(); previewController?.destroy(); if (clockTimer) window.clearInterval(clockTimer) })
</script>

<template>
  <main>
    <header class="topbar">
      <div><p class="eyebrow">RV1106 CONTROL PLANE</p><h1>AIPC Media Console</h1></div>
      <div class="connection"><span :class="['dot', connected && 'online']"></span>{{ connected ? 'SSE 已连接' : '正在重连' }}</div>
    </header>

    <div class="warning">初版未启用身份认证，仅应在可信局域网内使用。</div>
    <div v-if="notice" class="notice">{{ notice }}</div>
    <div v-if="error" class="error">{{ error }}</div>

    <section class="hero panel">
      <div><span class="label">WORKER STATE</span><div :class="['state', status?.state]">{{ status?.state || 'offline' }}</div><p>{{ status?.stage || '等待 daemon 状态' }}</p></div>
      <div class="hero-stats">
        <div><span>PID</span><strong>{{ status?.pid || '—' }}</strong></div>
        <div><span>Generation</span><strong>{{ short(status?.generation) }}</strong></div>
        <div><span>Started</span><strong>{{ startedAt }}</strong></div>
        <div><span>Uptime</span><strong>{{ uptime }}</strong></div>
        <div><span>Restarts</span><strong>{{ status?.restart_count ?? 0 }}</strong></div>
      </div>
      <div class="controls">
        <button :disabled="busy" @click="control('start')">Start</button>
        <button class="secondary" :disabled="busy" @click="control('stop')">Stop</button>
        <button class="accent" :disabled="busy" @click="control('restart')">Restart</button>
      </div>
    </section>

    <section class="panel preview-panel">
      <div class="section-head preview-head"><div><span class="label">LIVE PREVIEW / H264</span><h2>实时画面</h2><p>主码流 · WebSocket · jMuxer/MSE</p></div><div class="preview-actions"><span :class="['preview-state', preview.state]">{{ preview.state }}</span><button v-if="preview.state === 'disconnected' || preview.state === 'error'" class="accent" @click="connectPreview">连接</button><button v-else class="secondary" @click="disconnectPreview">断开</button></div></div>
      <div class="preview-stage">
        <video ref="previewVideo" autoplay muted playsinline></video>
        <div v-if="preview.state !== 'live'" class="preview-overlay"><strong>{{ preview.state === 'unsupported' ? '浏览器不支持 H264 MSE' : preview.state === 'error' ? '预览连接异常' : '等待 H264 关键帧' }}</strong><span>{{ preview.error || 'worker 就绪后会自动开始预览' }}</span></div>
      </div>
      <div class="preview-stats"><span>GEN <b>{{ short(preview.stream?.generation) }}</b></span><span>RES <b>{{ preview.stream ? `${preview.stream.width}×${preview.stream.height}` : '—' }}</b></span><span>RX FPS <b>{{ fmt(preview.receivedFps) }}</b></span><span>BITRATE <b>{{ fmt(preview.bitrateKbps) }} Kbps</b></span><span>RECEIVED <b>{{ bytes(preview.bytesReceived) }}</b></span><span>DROPS / RETRIES <b>{{ preview.droppedFrames }} / {{ preview.reconnects }}</b></span></div>
    </section>

    <section class="metrics-grid">
      <article class="panel metric-card"><div class="card-title"><span>VIDEO / H264</span><i :class="status?.video_ready && 'ready'"></i></div><div class="metric"><strong :class="videoFps === null && 'placeholder'">{{ videoFps === null ? videoMetricState : fmt(videoFps) }}</strong><span>{{ videoFps === null ? '' : 'FPS' }}</span></div><div class="mini"><span>码率 <b>{{ videoBitrate === null ? videoMetricState : `${fmt(videoBitrate)} Kbps` }}</b></span><span>包 <b>{{ videoMetrics?.packets ?? '等待指标' }}</b></span><span>关键帧 <b>{{ videoMetrics?.keyframes ?? '等待指标' }}</b></span><span>PTS <b>{{ videoMetrics?.last_pts ?? '等待指标' }}</b></span><span>超时 <b>{{ videoMetrics?.timeouts ?? '等待指标' }}</b></span><span>错误 <b>{{ videoMetrics?.errors ?? '等待指标' }}</b></span></div></article>
      <article class="panel metric-card"><div class="card-title"><span>AUDIO / G711A</span><i :class="status?.audio_ready && 'ready'"></i></div><div class="metric"><strong :class="audioBitrate === null && 'placeholder'">{{ audioBitrate === null ? audioMetricState : fmt(audioBitrate) }}</strong><span>{{ audioBitrate === null ? '' : 'Kbps' }}</span></div><div class="mini"><span>包 <b>{{ audioMetrics?.packets ?? audioMetricState }}</b></span><span>字节 <b>{{ audioMetrics?.bytes ?? audioMetricState }}</b></span><span>PTS <b>{{ audioMetrics?.last_pts ?? audioMetricState }}</b></span><span>超时 <b>{{ audioMetrics?.timeouts ?? audioMetricState }}</b></span><span>错误 <b>{{ audioMetrics?.errors ?? audioMetricState }}</b></span></div></article>
    </section>

    <section class="panel config" v-if="form">
      <div class="section-head"><div><span class="label">CONFIGURATION</span><h2>媒体参数</h2><p>保存后先预检，再执行冷重启；失败时自动回滚 last-good。</p></div><div><button class="secondary" @click="resetForm">撤销</button><button class="accent" :disabled="busy" @click="applyConfig">应用配置</button></div></div>
      <div class="form-grid">
        <label>分辨率<select v-model="form.video.width" @change="form.video.height = form.video.width === 1280 ? 720 : 1080"><option :value="1920">1920 × 1080</option><option :value="1280">1280 × 720</option></select></label>
        <label>FPS<input type="number" min="1" max="60" v-model.number="form.video.fps"></label>
        <label>H264 码率 (Kbps)<input type="number" min="64" max="50000" v-model.number="form.video.bitrate_kbps"></label>
        <label>GOP<input type="number" min="1" max="300" v-model.number="form.video.gop"></label>
        <label class="toggle"><input type="checkbox" v-model="form.audio.enabled"><span></span>启用音频</label>
      </div>
      <details><summary>高级硬件与超时配置</summary><div class="form-grid advanced">
        <label>IQ 目录<input v-model="form.isp.iq_dir"></label><label>声卡<input v-model="form.audio.card_name"></label>
        <label>VI device<input type="number" v-model.number="form.vi.device_id"></label><label>VI pipe<input type="number" v-model.number="form.vi.pipe_id"></label><label>VI channel<input type="number" v-model.number="form.vi.channel_id"></label><label>VI buffers<input type="number" v-model.number="form.vi.buffer_count"></label>
        <label>VPSS group<input type="number" v-model.number="form.vpss.group_id"></label><label>VPSS channel<input type="number" v-model.number="form.vpss.channel_id"></label><label>VENC channel<input type="number" v-model.number="form.video.venc_channel_id"></label><label>VENC buffers<input type="number" v-model.number="form.video.stream_buffer_count"></label>
        <label>Warning timeout<input type="number" v-model.number="form.runtime.warning_timeout_count"></label><label>Stalled timeout<input type="number" v-model.number="form.runtime.stalled_timeout_count"></label><label>Fatal timeout<input type="number" v-model.number="form.runtime.fatal_timeout_count"></label><label>Metrics (ms)<input type="number" v-model.number="form.runtime.metrics_interval_ms"></label>
        <label class="wide">视频输出<input v-model="form.video.output_path"></label><label class="wide">音频输出<input v-model="form.audio.output_path"></label>
      </div></details>
    </section>

    <section class="split">
      <article class="panel"><div class="section-head"><div><span class="label">CONFIG STATE</span><h2>配置版本</h2></div></div><div class="config-stack"><details v-for="row in configDiff" :key="row.name"><summary><b>{{ row.name }}</b><span>{{ short(row.value?.runtime.generation) }}</span></summary><pre>{{ JSON.stringify(row.value, null, 2) }}</pre></details></div><p v-if="configs?.last_error" class="rollback">最近回滚/错误：{{ configs.last_error }}</p></article>
      <article class="panel terminal"><div class="section-head"><div><span class="label">WORKER STDERR</span><h2>最近日志</h2></div><span>{{ live.logs.length }}/200</span></div><div class="log-lines"><p v-for="(line, i) in live.logs.slice(-80)" :key="i"><time>{{ new Date(line.timestamp_ms).toLocaleTimeString() }}</time> {{ line.line }}</p><p v-if="!live.logs.length" class="muted">暂无 stderr 输出</p></div></article>
    </section>

    <section class="panel events">
      <div class="section-head event-head">
        <div><span class="label">EVENT STREAM</span><h2>实时事件</h2><p>最近 {{ live.events.length }}/200 条 · 新事件追加到底部</p></div>
        <div class="event-tools">
          <div class="event-filters" aria-label="事件等级过滤">
            <button v-for="filter in eventFilters" :key="filter" class="secondary compact" :class="eventFilter === filter && 'selected'" @click="eventFilter = filter">{{ filter }}</button>
          </div>
          <button v-if="!eventFollow" class="secondary compact" @click="resumeEventFollow">继续跟随</button>
          <button class="secondary compact" @click="clearEvents">清空显示</button>
        </div>
      </div>
      <div ref="eventViewport" class="event-lines" @scroll="handleEventScroll">
        <details v-for="(event, index) in filteredEvents" :key="`${event.timestamp_ms}-${event.kind}-${index}`" :class="['event-row', `event-${eventLevel(event)}`]">
          <summary><time>{{ new Date(event.timestamp_ms).toLocaleTimeString() }}</time><span class="event-level">{{ eventLevel(event) }}</span><b>{{ eventName(event) }}</b><span class="event-summary">{{ eventSummary(event) }}</span></summary>
          <pre>{{ JSON.stringify(event.payload, null, 2) }}</pre>
        </details>
        <p v-if="!filteredEvents.length" class="event-empty">当前过滤条件下暂无事件</p>
      </div>
    </section>
  </main>
</template>
