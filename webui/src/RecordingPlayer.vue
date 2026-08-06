<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue'
import type { RecordingEntry } from './types'
import { PLAYBACK_RATES, clampPlaybackTime, formatPlaybackTime } from './recordingPlayer'

const props = defineProps<{ recording: RecordingEntry }>()
const emit = defineEmits<{ close: [] }>()
const video = ref<HTMLVideoElement | null>(null)
const audio = ref<HTMLAudioElement | null>(null)
const shell = ref<HTMLElement | null>(null)
const playing = ref(false)
const buffering = ref(false)
const current = ref(0)
const duration = ref(0)
const volume = ref(1)
const speed = ref(1)
const error = ref('')
const source = computed(() => `/api/v1/recordings/${encodeURIComponent(props.recording.id)}/content`)
const audioSource = computed(() => `/api/v1/recordings/${encodeURIComponent(props.recording.id)}/audio`)

async function toggle() { if (!video.value) return; if (video.value.paused) await video.value.play(); else video.value.pause() }
function seek(value: number) { if (video.value) video.value.currentTime = value; if (audio.value) audio.value.currentTime = value }
function skip(delta: number) { if (video.value) seek(clampPlaybackTime(video.value.currentTime, delta, duration.value)) }
function setSpeed(value: number) { speed.value = value; if (video.value) video.value.playbackRate = value; if (audio.value) audio.value.playbackRate = value }
function setVolume(value: number) { volume.value = value; if (audio.value) audio.value.volume = value }
function mute() { setVolume(volume.value > 0 ? 0 : 1) }
async function fullscreen() { if (shell.value?.requestFullscreen) await shell.value.requestFullscreen() }
function close() { cleanup(); emit('close') }
function cleanup() { video.value?.pause(); audio.value?.pause(); if (video.value) { video.value.removeAttribute('src'); video.value.load() }; if (audio.value) { audio.value.removeAttribute('src'); audio.value.load() } }
async function syncPlay() {
  if (!audio.value || !props.recording.audio_available || !video.value) return
  if (Math.abs(audio.value.currentTime - video.value.currentTime) > 0.08) audio.value.currentTime = video.value.currentTime
  audio.value.playbackRate = video.value.playbackRate
  try { await audio.value.play() } catch { error.value = '录像音频播放失败，视频将继续静音播放' }
}
function syncPause() { audio.value?.pause() }
function keepSynced() { if (audio.value && video.value && Math.abs(audio.value.currentTime - video.value.currentTime) > 0.2) audio.value.currentTime = video.value.currentTime }

watch(() => props.recording.id, async () => {
  cleanup(); error.value = ''; current.value = 0; duration.value = 0
  await nextTick(); if (video.value) { video.value.src = source.value; video.value.load() }
})
onBeforeUnmount(cleanup)
</script>

<template>
  <div class="player-backdrop" @click.self="close">
    <section ref="shell" class="recording-player">
      <header><div><span class="label">MP4 PLAYBACK</span><h3>{{ recording.file_name }}</h3></div><button class="secondary compact" @click="close">关闭</button></header>
      <div class="player-stage">
        <video ref="video" :src="source" playsinline preload="metadata"
          @play="playing = true; syncPlay()" @pause="playing = false; syncPause()" @waiting="buffering = true" @playing="buffering = false"
          @timeupdate="current = video?.currentTime || 0; keepSynced()" @seeked="audio && (audio.currentTime = video?.currentTime || 0)" @durationchange="duration = video?.duration || 0"
          @ended="playing = false; syncPause()" @ratechange="audio && (audio.playbackRate = video?.playbackRate || 1)" @error="error = '浏览器无法播放该 MP4/H.264 文件'" @click="toggle" />
        <audio v-if="recording.audio_available" ref="audio" :src="audioSource" preload="auto" @error="error = 'WAV 音频不可用，视频将静音播放'" />
        <div v-if="buffering" class="player-message"><span class="loader"></span>正在缓冲</div>
        <div v-if="error" class="player-message error-text">{{ error }}</div><div v-else-if="!recording.audio_available" class="player-message">该录像没有可用音频</div>
      </div>
      <div class="player-controls">
        <button class="accent compact" @click="toggle">{{ playing ? '暂停' : current >= duration && duration ? '重播' : '播放' }}</button>
        <button class="secondary compact" @click="skip(-10)">−10s</button><button class="secondary compact" @click="skip(10)">+10s</button>
        <span>{{ formatPlaybackTime(current) }}</span>
        <input class="timeline" type="range" min="0" :max="duration || 0" step="0.01" :value="current" @input="seek(Number(($event.target as HTMLInputElement).value))">
        <span>{{ formatPlaybackTime(duration) }}</span>
        <button class="secondary compact" @click="mute">{{ volume ? '静音' : '有声' }}</button>
        <input class="volume" type="range" min="0" max="1" step="0.05" :value="volume" @input="setVolume(Number(($event.target as HTMLInputElement).value))">
        <select :value="speed" @change="setSpeed(Number(($event.target as HTMLSelectElement).value))"><option v-for="rate in PLAYBACK_RATES" :key="rate" :value="rate">{{ rate }}×</option></select>
        <button class="secondary compact" @click="fullscreen">全屏</button>
        <a class="download-link" :href="`/api/v1/recordings/${encodeURIComponent(recording.id)}/download`">下载</a>
      </div>
    </section>
  </div>
</template>
