<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { api, connectAiResultEvents } from './api'
import { aiResultCategory, aiResultMatches, aiResultSummary, aiResultTypeLabel, appendAiResultEvents, shortEventId } from './aiResults'
import type { AiResultFilter } from './aiResults'
import type { AiCloudEvent, AiModelInfo, AiOsdMode, AiProjectDocument, AiProjectSummary, AiStatus } from './types'

const status = ref<AiStatus | null>(null)
const projects = ref<AiProjectSummary[]>([])
const models = ref<AiModelInfo[]>([])
const selectedId = ref('')
const document = ref<AiProjectDocument | null>(null)
const busy = ref(false)
const notice = ref('')
const error = ref('')
const upload = ref<HTMLInputElement | null>(null)
const resultEvents = ref<AiCloudEvent[]>([])
const resultFilter = ref<AiResultFilter>('all')
const resultConnected = ref(false)
const resultPaused = ref(false)
const resultSkipped = ref(0)
const pendingResultEvents: AiCloudEvent[] = []
let timer = 0
let resultTimer = 0
let disconnectResults: (() => void) | null = null

const selected = computed(() => projects.value.find((project) => project.id === selectedId.value))
const visibleResultEvents = computed(() => resultEvents.value.filter((event) => aiResultMatches(event, resultFilter.value)))

async function refresh() {
  try {
    const [nextStatus, nextProjects, nextModels] = await Promise.all([api.aiStatus(), api.aiProjects(), api.aiModels()])
    status.value = nextStatus
    projects.value = nextProjects
    models.value = nextModels
    if (!selectedId.value && nextProjects.length) await selectProject(nextProjects[0].id)
  } catch (cause) { error.value = String(cause) }
}

async function selectProject(id: string) {
  selectedId.value = id
  document.value = await api.aiProject(id)
}

function newProject() {
  let suffix = 1
  while (projects.value.some((project) => project.id === `project-${suffix}`)) suffix += 1
  selectedId.value = ''
  document.value = {
    manifest: {
      schema_version: 2,
      id: `project-${suffix}`,
      name: `AI Project ${suffix}`,
      entry: 'main.lua',
      algorithm: 'yolov5',
      model: models.value.find((model) => model.name.endsWith('.rknn'))?.name || '',
      labels: models.value.find((model) => model.name.endsWith('.txt'))?.name || '',
      files: {},
      options: {},
      input: { enabled: true, channel_id: 1, width: 640, height: 640, fps: 10, pixel_format: 'nv12', fit_mode: 'contain', buffer_count: 2, depth: 1 },
      threshold: 0.25,
      nms_threshold: 0.45,
      max_detections: 24,
      class_filter: [],
    },
    script: `local model\n\nfunction init(config)\n  model = aipc.load_model(config.model, config)\nend\n\nfunction process(frame)\n  return aipc.detect(frame, model, {})\nend\n`,
  }
}

async function save() {
  if (!document.value) return
  await action(async () => {
    const exists = projects.value.some((project) => project.id === document.value!.manifest.id)
    document.value = exists ? await api.saveAiProject(document.value!) : await api.createAiProject(document.value!)
    selectedId.value = document.value.manifest.id
    notice.value = '项目已原子保存'
    await refresh()
  })
}

async function validateProject() {
  if (!document.value) return
  await save()
  await action(async () => {
    await api.validateAiProject(document.value!.manifest.id)
    notice.value = 'manifest、Lua 和模型校验通过'
  })
}

async function deploy() {
  if (!document.value || !confirm('部署会在线重配 AI VPSS 并仅重启 ai_worker，是否继续？')) return
  await save()
  await action(async () => {
    status.value = await api.deployAiProject(document.value!.manifest.id)
    notice.value = '候选项目首次推理成功，已更新 last-good'
    await refresh()
  })
}

async function removeProject() {
  if (!selected.value || !confirm(`删除项目 ${selected.value.id}？`)) return
  await action(async () => {
    await api.deleteAiProject(selected.value!.id)
    selectedId.value = ''
    document.value = null
    notice.value = '项目已删除'
    await refresh()
  })
}

async function uploadModel() {
  const file = upload.value?.files?.[0]
  if (!file) return
  await action(async () => {
    await api.uploadAiModel(file)
    if (upload.value) upload.value.value = ''
    notice.value = '模型已校验并原子上传'
    await refresh()
  })
}

async function removeModel(model: AiModelInfo) {
  if (!confirm(`删除 ${model.name}？`)) return
  await action(async () => {
    await api.deleteAiModel(model.name)
    notice.value = '模型已删除'
    await refresh()
  })
}

async function setOsd(mode: AiOsdMode) {
  await action(async () => {
    await api.setAiOsd(mode)
    notice.value = `OSD 已切换为 ${mode}`
    await refresh()
  })
}

function setClassFilter(value: string) {
  if (!document.value) return
  document.value.manifest.class_filter = value.split(',').map((item) => Number(item.trim())).filter(Number.isInteger)
}

function setManifestJson(field: 'files' | 'options', value: string) {
  if (!document.value) return
  const parsed = JSON.parse(value || '{}')
  if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object') throw new Error(`${field} 必须是 JSON 对象`)
  document.value.manifest[field] = parsed
}

function manifestJson(field: 'files' | 'options') {
  return JSON.stringify(document.value?.manifest[field] || {}, null, 2)
}

async function action(operation: () => Promise<void>) {
  busy.value = true
  error.value = ''
  notice.value = ''
  try { await operation() } catch (cause) { error.value = String(cause) } finally { busy.value = false }
}

function enqueueResult(event: AiCloudEvent) {
  if (resultPaused.value) {
    resultSkipped.value += 1
    return
  }
  pendingResultEvents.push(event)
}

function flushResultEvents() {
  if (!pendingResultEvents.length) return
  resultEvents.value = appendAiResultEvents(resultEvents.value, pendingResultEvents.splice(0))
}

async function toggleResultPause() {
  resultPaused.value = !resultPaused.value
  if (!resultPaused.value) {
    const latest = await api.aiResultLatest().catch(() => null)
    if (latest) enqueueResult(latest)
    flushResultEvents()
  }
}

function clearResultEvents() {
  pendingResultEvents.splice(0)
  resultEvents.value = []
  resultSkipped.value = 0
}

function eventTime(event: AiCloudEvent) {
  const time = new Date(event.time)
  return Number.isNaN(time.getTime()) ? event.time : time.toLocaleTimeString()
}

onMounted(() => {
  void refresh()
  timer = window.setInterval(() => { void api.aiStatus().then((value) => { status.value = value }).catch(() => undefined) }, 2000)
  resultTimer = window.setInterval(flushResultEvents, 120)
  void api.aiResultLatest().then((event) => { if (event) enqueueResult(event) }).catch(() => undefined)
  disconnectResults = connectAiResultEvents(enqueueResult, (connected) => { resultConnected.value = connected })
})
onBeforeUnmount(() => {
  window.clearInterval(timer)
  window.clearInterval(resultTimer)
  disconnectResults?.()
})
</script>

<template>
  <div class="ai-page">
    <div v-if="notice" class="notice">{{ notice }}</div>
    <div v-if="error" class="error">{{ error }}</div>
    <section class="panel ai-status-panel">
      <div class="section-head"><div><span class="label">AI RUNTIME</span><h3>独立 AI Worker</h3><p>AI 失败、降帧或重启不会进入主 VENC 关键路径。</p></div><span :class="['status-pill', status?.state]">{{ status?.state || 'offline' }}</span></div>
      <div class="ai-status-grid">
        <div><span>项目</span><b>{{ status?.active_project || '—' }}</b></div><div><span>Last-good</span><b>{{ status?.last_good_project || '—' }}</b></div>
        <div><span>PID / generation</span><b>{{ status?.pid || '—' }} / {{ status?.generation?.slice(0, 8) || '—' }}</b></div><div><span>输入</span><b>{{ status?.input.width || '—' }} × {{ status?.input.height || '—' }}</b></div>
        <div><span>推理</span><b>{{ status?.inference_fps.toFixed(1) }} FPS · {{ status?.average_inference_ms.toFixed(1) }} ms</b></div><div><span>帧 / 结果</span><b>{{ status?.input.frames_received || 0 }} / {{ status?.results || 0 }}</b></div>
      </div>
      <p v-if="status?.last_error" class="rollback">最近错误：{{ status.last_error }}</p>
      <div class="osd-modes"><span>全局 OSD 模式</span><button v-for="mode in (['off', 'metadata', 'embedded_rgn'] as AiOsdMode[])" :key="mode" class="secondary compact" :class="status?.osd_mode === mode && 'selected'" :disabled="busy" @click="setOsd(mode)">{{ mode }}</button></div>
    </section>

    <section class="ai-workspace">
      <article class="panel ai-project-list">
        <div class="section-head"><div><span class="label">LUA PROJECTS</span><h3>项目</h3></div><button class="accent compact" @click="newProject">新建</button></div>
        <button v-for="project in projects" :key="project.id" class="project-item" :class="selectedId === project.id && 'active'" @click="selectProject(project.id)"><b>{{ project.name }}</b><small>{{ project.id }} · {{ project.input.width }}×{{ project.input.height }} @ {{ project.input.fps }}</small><span>{{ project.active ? 'ACTIVE' : project.last_good ? 'LAST-GOOD' : project.algorithm }}</span></button>
        <p v-if="!projects.length" class="empty-state">暂无项目</p>
      </article>

      <article v-if="document" class="panel ai-editor">
        <div class="section-head"><div><span class="label">MANIFEST + LUA</span><h3>{{ document.manifest.name }}</h3></div><div class="section-actions"><button class="secondary" :disabled="busy || selected?.active || selected?.last_good" @click="removeProject">删除</button><button class="secondary" :disabled="busy" @click="validateProject">校验</button><button :disabled="busy" @click="save">保存</button><button class="accent" :disabled="busy" @click="deploy">部署</button></div></div>
        <div class="form-grid ai-manifest">
          <label>项目 ID<input v-model="document.manifest.id" :disabled="Boolean(selected)"></label><label>名称<input v-model="document.manifest.name"></label><label>算法<select v-model="document.manifest.algorithm"><option v-for="algorithm in ['yolov5', 'yolo11', 'lprnet', 'mlsd', 'ppocr', 'nanotrack', 'find_blobs', 'ive_filter', 'ive_ncc', 'npu_clock', 'frame_info']" :key="algorithm" :value="algorithm">{{ algorithm }}</option></select></label><label>模型 / 主资源<select v-model="document.manifest.model"><option value="">无</option><option v-for="model in models" :key="model.name" :value="model.name">{{ model.name }}</option></select></label><label>标签<select v-model="document.manifest.labels"><option value="">无</option><option v-for="model in models" :key="model.name" :value="model.name">{{ model.name }}</option></select></label>
          <label>AI VPSS 通道<input type="number" min="0" max="3" v-model.number="document.manifest.input.channel_id"></label><label>输入宽度<input type="number" min="2" step="2" v-model.number="document.manifest.input.width"></label><label>输入高度<input type="number" min="2" step="2" v-model.number="document.manifest.input.height"></label><label>抓帧 FPS<input type="number" min="1" max="60" v-model.number="document.manifest.input.fps"></label>
          <label>Fit mode<select v-model="document.manifest.input.fit_mode"><option value="contain">contain / letterbox</option><option value="cover">cover / crop</option><option value="stretch">stretch</option></select></label><label>Buffer<input type="number" min="1" max="8" v-model.number="document.manifest.input.buffer_count"></label><label>Depth<input type="number" min="1" max="8" v-model.number="document.manifest.input.depth"></label><label>最大框数<input type="number" min="1" max="256" v-model.number="document.manifest.max_detections"></label>
          <label>置信度<input type="number" min="0" max="1" step="0.01" v-model.number="document.manifest.threshold"></label><label>NMS<input type="number" min="0" max="1" step="0.01" v-model.number="document.manifest.nms_threshold"></label><label class="wide">类别 ID（逗号分隔，留空为全部）<input :value="document.manifest.class_filter.join(',')" @change="setClassFilter(($event.target as HTMLInputElement).value)"></label>
          <label class="wide">附加资源 JSON<textarea :value="manifestJson('files')" @change="setManifestJson('files', ($event.target as HTMLTextAreaElement).value)"></textarea></label><label class="wide">算法参数 JSON<textarea :value="manifestJson('options')" @change="setManifestJson('options', ($event.target as HTMLTextAreaElement).value)"></textarea></label>
        </div>
        <label class="lua-editor-label">main.lua<textarea v-model="document.script" spellcheck="false"></textarea></label>
      </article>
    </section>

    <section class="panel ai-models">
      <div class="section-head"><div><span class="label">AI RESOURCES</span><h3>模型与资源</h3><p>上传使用临时文件和原子 rename；活动或 last-good 项目引用的文件不可删除。</p></div><div class="model-upload"><input ref="upload" type="file" accept=".rknn,.txt,.jpg,.jpeg,.png,.ttf,.calib"><button class="accent" :disabled="busy" @click="uploadModel">上传</button></div></div>
      <div class="model-row model-header"><span>文件</span><span>大小</span><span>SHA-256</span><span>状态</span><span></span></div>
      <div v-for="model in models" :key="model.name" class="model-row"><b>{{ model.name }}</b><span>{{ (model.bytes / 1024 / 1024).toFixed(2) }} MiB</span><code>{{ model.sha256 }}</code><span>{{ model.active ? 'IN USE' : 'AVAILABLE' }}</span><button class="secondary compact" :disabled="model.active || busy" @click="removeModel(model)">删除</button></div>
    </section>

    <section class="panel ai-results-panel">
      <div class="section-head event-head">
        <div><span class="label">STANDARD AI EVENTS</span><h3>结构化识别事件</h3><p>CloudEvents 1.0 实时结果；展开事件可查看完整标准 JSON，适用于报警和记录服务联调。</p></div>
        <div class="section-actions result-actions">
          <span :class="['result-connection', resultConnected && 'online']"><i></i>{{ resultConnected ? 'LIVE' : 'RECONNECTING' }}</span>
          <button class="secondary compact" :class="resultPaused && 'selected'" @click="toggleResultPause">{{ resultPaused ? '继续' : '暂停' }}</button>
          <button class="secondary compact" @click="clearResultEvents">清空</button>
        </div>
      </div>
      <div class="result-bus-grid">
        <div><span>Stream</span><b>{{ status?.result_bus.stream_id.slice(0, 8) || '—' }}</b></div>
        <div><span>Latest</span><b>{{ shortEventId(status?.result_bus.latest_event_id) }}</b></div>
        <div><span>Replay</span><b>{{ status?.result_bus.replay_depth || 0 }} / {{ status?.result_bus.replay_capacity || 0 }}</b></div>
        <div><span>Published</span><b>{{ status?.result_bus.published || 0 }}</b></div>
        <div><span>Subscriber lag</span><b>{{ status?.result_bus.lagged_events || 0 }}</b></div>
      </div>
      <div class="result-toolbar">
        <label>事件类型<select v-model="resultFilter"><option value="all">全部事件</option><option value="frame">逐帧结果</option><option value="tracks">目标生命周期</option><option value="generation">Generation</option><option value="gap">Replay gap</option></select></label>
        <span>{{ visibleResultEvents.length }} / {{ resultEvents.length }} events</span>
        <span v-if="resultSkipped">暂停期间跳过 {{ resultSkipped }}</span>
        <a href="/api/v1/ai/results/schema" target="_blank" rel="noreferrer">查看 JSON Schema ↗</a>
      </div>
      <div class="ai-result-events">
        <details v-for="event in visibleResultEvents" :key="event.id" :class="['ai-result-row', `result-${aiResultCategory(event)}`, event.type.includes('.exited.') && 'result-exited']">
          <summary>
            <time>{{ eventTime(event) }}</time>
            <span class="result-type">{{ aiResultTypeLabel(event.type) }}</span>
            <b>{{ event.subject }}</b>
            <span class="result-summary">{{ aiResultSummary(event) }}</span>
            <code>{{ shortEventId(event.id) }}</code>
          </summary>
          <pre>{{ JSON.stringify(event, null, 2) }}</pre>
        </details>
        <p v-if="!visibleResultEvents.length" class="empty-state">等待标准化 AI 事件…</p>
      </div>
    </section>
  </div>
</template>
