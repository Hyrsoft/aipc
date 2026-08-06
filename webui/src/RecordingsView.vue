<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { api } from './api'
import type { RecordingEntry, RecordingSettings, RecordingStatus, RtspStatus } from './types'
import RecordingPlayer from './RecordingPlayer.vue'

const settings = ref<RecordingSettings | null>(null)
const status = ref<RecordingStatus | null>(null)
const rtsp = ref<RtspStatus | null>(null)
const files = ref<RecordingEntry[]>([])
const total = ref(0)
const directory = ref('')
const selected = ref<string[]>([])
const playing = ref<RecordingEntry | null>(null)
const busy = ref(false)
const error = ref('')
const notice = ref('')
const hostName = window.location.hostname
let timer: number | undefined
const allSelected = computed(() => files.value.length > 0 && files.value.every(file => selected.value.includes(file.id)))

function bytes(value: number) { if (value >= 1024 ** 3) return `${(value / 1024 ** 3).toFixed(2)} GiB`; if (value >= 1024 ** 2) return `${(value / 1024 ** 2).toFixed(1)} MiB`; return `${(value / 1024).toFixed(1)} KiB` }
function duration(ms: number) { const seconds = Math.floor(ms / 1000); return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}` }
function toggleAll() { selected.value = allSelected.value ? [] : files.value.map(file => file.id) }
function toggle(id: string) { selected.value = selected.value.includes(id) ? selected.value.filter(item => item !== id) : [...selected.value, id] }

async function refresh() {
  try {
    const [nextSettings, nextStatus, list, nextRtsp] = await Promise.all([api.recordingSettings(), api.recordingStatus(), api.recordings(), api.rtspStatus()])
    settings.value = nextSettings; status.value = nextStatus; files.value = list.items; total.value = list.total; rtsp.value = nextRtsp
    if (!directory.value) directory.value = nextSettings.directory
    selected.value = selected.value.filter(id => list.items.some(item => item.id === id))
  } catch (cause) { error.value = String(cause) }
}
async function saveDirectory() { busy.value = true; error.value = ''; try { settings.value = await api.updateRecordingSettings(directory.value); notice.value = '录像目录已保存' } catch (cause) { error.value = String(cause) } finally { busy.value = false } }
async function control(action: 'start' | 'stop') { busy.value = true; error.value = ''; try { status.value = await api.recordingControl(action); notice.value = action === 'start' ? '正在等待关键帧开始录像' : '正在完成 MP4 与 WAV 文件'; await refresh() } catch (cause) { error.value = String(cause) } finally { busy.value = false } }
async function removeSelected() { if (!selected.value.length || !confirm(`确认删除 ${selected.value.length} 个录像文件？此操作不可恢复。`)) return; busy.value = true; try { const result = await api.deleteRecordings(selected.value); notice.value = `已删除 ${result.deleted} 个录像`; selected.value = []; await refresh() } catch (cause) { error.value = String(cause) } finally { busy.value = false } }
async function exportSelected() { if (!selected.value.length) return; busy.value = true; try { const blob = await api.exportRecordings(selected.value); const url = URL.createObjectURL(blob); const link = document.createElement('a'); link.href = url; link.download = `aipc-recordings-${Date.now()}.zip`; link.click(); window.setTimeout(() => URL.revokeObjectURL(url), 30_000) } catch (cause) { error.value = String(cause) } finally { busy.value = false } }

onMounted(() => { refresh(); timer = window.setInterval(refresh, 2000) })
onBeforeUnmount(() => { if (timer) window.clearInterval(timer) })
</script>

<template>
  <div class="recordings-page">
    <div v-if="notice" class="notice">{{ notice }}</div><div v-if="error" class="error">{{ error }}</div>
    <section class="panel recording-control">
      <div class="section-head"><div><span class="label">RECORDING</span><h3>录像控制</h3><p>录像由 Rust daemon 封装为 MP4，播放时由浏览器解码。</p></div><div class="section-actions"><span :class="['recording-state', status?.state]">{{ status?.state || 'offline' }}</span><button v-if="status?.state === 'idle' || status?.state === 'failed'" class="accent" :disabled="busy" @click="control('start')">开始录像</button><button v-else class="secondary danger" :disabled="busy" @click="control('stop')">停止录像</button></div></div>
      <div class="recording-summary"><div><span>当前文件</span><b>{{ status?.file_name || '—' }}</b></div><div><span>时长</span><b>{{ duration(status?.duration_ms || 0) }}</b></div><div><span>大小</span><b>{{ bytes(status?.bytes || 0) }}</b></div><div><span>RTSP</span><b>{{ rtsp?.listening ? `rtsp://${hostName}:${rtsp.bind.split(':').at(-1)}${rtsp.path}` : '未监听' }}</b></div></div>
      <div class="storage-setting"><label>设备录像目录<input v-model="directory" placeholder="/mnt/storage/recordings"></label><button class="secondary" :disabled="busy || status?.state === 'recording'" @click="saveDirectory">保存目录</button></div>
    </section>
    <section class="panel recordings-list">
      <div class="section-head"><div><span class="label">LIBRARY</span><h3>录像文件 · {{ total }}</h3></div><div class="section-actions"><button class="secondary" @click="toggleAll">{{ allSelected ? '取消全选' : '全选' }}</button><button class="secondary" :disabled="!selected.length || busy" @click="exportSelected">导出 ZIP</button><button class="secondary danger" :disabled="!selected.length || busy" @click="removeSelected">删除</button></div></div>
      <div class="recording-table"><div class="recording-row recording-header"><span></span><span>文件</span><span>录制时间</span><span>时长</span><span>大小</span><span>画面 / 音频</span><span>操作</span></div><div v-for="file in files" :key="file.id" class="recording-row"><input type="checkbox" :checked="selected.includes(file.id)" @change="toggle(file.id)"><button class="file-button" @click="playing = file">{{ file.file_name }}</button><span>{{ new Date(file.created_at_ms).toLocaleString() }}</span><span>{{ duration(file.duration_ms) }}</span><span>{{ bytes(file.bytes) }}<small v-if="file.audio_available"> + {{ bytes(file.audio_bytes) }}</small></span><span>{{ file.width }}×{{ file.height }} · {{ file.fps }}fps<br>{{ file.audio_available ? `${file.audio_sample_rate} Hz · ${file.audio_channels}ch` : '静音' }}</span><span class="row-actions"><button class="accent compact" @click="playing = file">播放</button><a :href="`/api/v1/recordings/${encodeURIComponent(file.id)}/download`">下载</a></span></div><p v-if="!files.length" class="empty-state">还没有已完成的录像</p></div>
    </section>
    <RecordingPlayer v-if="playing" :recording="playing" @close="playing = null" />
  </div>
</template>
