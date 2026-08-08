<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { api } from './api'
import type { DependencyInfo, DependencyList, DependencyVersion } from './types'

const dependencies = ref<DependencyList | null>(null)
const busyId = ref('')
const error = ref('')
const notice = ref('')
const uploadInput = ref<HTMLInputElement | null>(null)
const uploadTarget = ref('')
let timer = 0

function bytes(value: number) {
  if (value >= 1024 * 1024) return `${(value / 1024 / 1024).toFixed(1)} MiB`
  return `${(value / 1024).toFixed(1)} KiB`
}

function short(value: string | null | undefined) { return value ? value.slice(0, 12) : '—' }
function versionLabel(version: DependencyVersion | null) {
  if (!version) return '不可用'
  return version.detected_version || (version.build_id ? `build ${short(version.build_id)}` : short(version.sha256))
}
function isActive(item: DependencyInfo, version: DependencyVersion) { return item.active?.sha256 === version.sha256 }
function isPrevious(item: DependencyInfo, version: DependencyVersion) { return item.previous?.sha256 === version.sha256 }

async function refresh() {
  try {
    dependencies.value = await api.dependencies()
    error.value = ''
  } catch (cause) { error.value = String(cause) }
}

function chooseUpload(id: string) {
  uploadTarget.value = id
  uploadInput.value?.click()
}

async function upload() {
  const file = uploadInput.value?.files?.[0]
  const id = uploadTarget.value
  if (!file || !id) return
  await action(id, async () => {
    const version = await api.uploadDependency(id, file)
    notice.value = `${id} 候选版本已校验：${short(version.sha256)}，请确认后激活`
    if (uploadInput.value) uploadInput.value.value = ''
    await refresh()
  })
}

async function activate(item: DependencyInfo, version: DependencyVersion) {
  if (isActive(item, version) || !confirmDanger(item, `激活 ${versionLabel(version)} 会冷重启 ${item.owners.join('、')}，是否继续？`)) return
  await action(item.id, async () => {
    await api.activateDependency(item.id, version.sha256)
    notice.value = `${item.id} 已激活，worker 健康检查通过`
    await refresh()
  })
}

async function rollback(item: DependencyInfo) {
  if (!item.previous || !confirmDanger(item, `回滚 ${versionLabel(item.previous)} 并冷重启受影响 worker？`)) return
  await action(item.id, async () => {
    await api.rollbackDependency(item.id)
    notice.value = `${item.id} 已回滚`
    await refresh()
  })
}

async function restoreFactory(item: DependencyInfo) {
  if (!confirmDanger(item, '恢复 factory 库并冷重启受影响 worker？')) return
  await action(item.id, async () => {
    await api.restoreDependencyFactory(item.id)
    notice.value = `${item.id} 已恢复 factory`
    await refresh()
  })
}

async function remove(item: DependencyInfo, version: DependencyVersion) {
  if (isActive(item, version) || isPrevious(item, version) || !confirm(`删除未使用版本 ${short(version.sha256)}？`)) return
  await action(item.id, async () => {
    await api.deleteDependencyVersion(item.id, version.sha256)
    notice.value = '未使用版本已删除'
    await refresh()
  })
}

function confirmDanger(item: DependencyInfo, message: string) {
  if (!dependencies.value?.enabled) return false
  if (!confirm(`高风险操作：共享库上传等同执行原生代码。\n${message}`)) return false
  return window.prompt(`请输入库 ID「${item.id}」确认操作`) === item.id
}

async function action(id: string, operation: () => Promise<void>) {
  busyId.value = id
  error.value = ''
  notice.value = ''
  try { await operation() } catch (cause) { error.value = String(cause) } finally { busyId.value = '' }
}

onMounted(() => {
  void refresh()
  timer = window.setInterval(() => { void refresh() }, 2000)
})
onBeforeUnmount(() => window.clearInterval(timer))
</script>

<template>
  <section class="panel dependency-panel">
    <div class="section-head">
      <div><span class="label">DEPENDENCY RUNTIME</span><h3>依赖库管理</h3><p>白名单库由 daemon 管理；激活后只冷重启受影响子进程，失败会自动回滚。</p></div>
      <span :class="['status-pill', dependencies?.enabled ? 'running' : '']">{{ dependencies?.enabled ? 'ENABLED' : 'DISABLED' }}</span>
    </div>
    <div v-if="notice" class="dependency-notice">{{ notice }}</div>
    <div v-if="error" class="dependency-error">{{ error }}</div>
    <div v-if="!dependencies" class="empty-state">读取依赖库状态…</div>
    <template v-else>
      <div v-if="!dependencies.enabled" class="dependency-disabled">依赖库替换接口默认关闭。编辑 daemon 配置中的 <code>dependencies.enabled</code> 并重启 daemon 后启用。</div>
      <div class="dependency-list">
        <article v-for="item in dependencies.items" :key="item.id" class="dependency-card">
          <div class="dependency-card-head"><div><b>{{ item.display_name }}</b><small>{{ item.id }} · {{ item.load_names.join(', ') }}</small></div><span :class="['dependency-state', `dependency-${item.state}`]">{{ item.state }}</span></div>
          <div class="dependency-owners"><span v-for="owner in item.owners" :key="owner">{{ owner }}</span></div>
          <div class="dependency-version-grid">
            <div><span>ACTIVE</span><b>{{ versionLabel(item.active) }}</b><code>{{ short(item.active?.sha256) }}</code></div>
            <div><span>FACTORY</span><b>{{ versionLabel(item.factory) }}</b><code>{{ short(item.factory?.sha256) }}</code></div>
            <div><span>PREVIOUS</span><b>{{ versionLabel(item.previous) }}</b><code>{{ short(item.previous?.sha256) }}</code></div>
          </div>
          <p v-if="item.last_error" class="dependency-last-error">{{ item.last_error }}</p>
          <div class="dependency-actions"><button class="accent compact" :disabled="!dependencies.enabled || busyId === item.id" @click="chooseUpload(item.id)">上传候选</button><button class="secondary compact" :disabled="!dependencies.enabled || !item.previous || busyId === item.id" @click="rollback(item)">回滚</button><button class="secondary compact" :disabled="!dependencies.enabled || busyId === item.id" @click="restoreFactory(item)">恢复 factory</button></div>
          <div v-if="item.versions.length" class="dependency-versions"><div v-for="version in item.versions" :key="version.sha256" class="dependency-version-row"><div><b>{{ versionLabel(version) }}</b><small>{{ bytes(version.bytes) }} · {{ short(version.sha256) }} · {{ version.source }}</small></div><div class="dependency-version-actions"><span v-if="isActive(item, version)" class="dependency-tag">ACTIVE</span><span v-else-if="isPrevious(item, version)" class="dependency-tag previous">PREVIOUS</span><button v-else class="secondary compact" :disabled="busyId === item.id" @click="remove(item, version)">删除</button><button v-if="!isActive(item, version)" class="secondary compact" :disabled="!dependencies.enabled || busyId === item.id" @click="activate(item, version)">激活</button></div></div></div>
          <p v-else class="empty-state">暂无上传版本，当前使用 factory</p>
        </article>
      </div>
    </template>
    <input ref="uploadInput" class="dependency-file-input" type="file" @change="upload">
  </section>
</template>
