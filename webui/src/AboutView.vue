<script setup lang="ts">
import { computed } from 'vue'
import type { AboutInfo } from './types'

const props = defineProps<{ info: AboutInfo | null }>()

const links = computed(() => props.info ? [
  { label: '项目源码', description: 'AIPC GitHub 仓库、Issue 和版本历史', url: props.info.project_url },
  { label: '架构文档', description: '进程边界、构建、部署和 AI 协议说明', url: props.info.documentation_url },
  { label: 'VisionG', description: '当前 RKNN 推理后端使用的 C++ 视觉库', url: props.info.visiong_url },
  { label: props.info.license_name, description: 'AIPC 项目的开源许可证', url: props.info.license_url },
] : [])
</script>

<template>
  <div class="about-page">
    <section class="panel about-hero">
      <div>
        <span class="label">ABOUT {{ info?.project_name || 'AIPC' }}</span>
        <h3>面向边缘设备的音视频与 AI 控制平面</h3>
        <p>AIPC 将实时媒体链路、AI 推理和 Web 管理拆成相互隔离的进程。主视频连续性优先，AI 可以独立降帧、重启或切换模型，而不会进入 VENC 的同步关键路径。</p>
      </div>
      <div class="about-badge">
        <span>{{ info?.project_name || 'AIPC' }}</span>
        <strong>v{{ info?.daemon_version || '—' }}</strong>
        <small>{{ info?.console_name || 'Media Console' }}</small>
      </div>
    </section>

    <section class="about-grid">
      <article class="panel about-platform">
        <div class="section-head"><div><span class="label">CURRENT TARGET</span><h3>当前运行平台</h3><p>平台标识由 daemon 配置文件提供，后续适配其他 SoC 时无需修改 Web 源码。</p></div></div>
        <dl>
          <div><dt>Platform</dt><dd>{{ info?.platform_name || '—' }}</dd></div>
          <div><dt>Board</dt><dd>{{ info?.board_name || '—' }}</dd></div>
          <div><dt>Daemon</dt><dd>{{ info?.daemon_version || '—' }}</dd></div>
          <div><dt>Security</dt><dd>Trusted LAN / No Auth</dd></div>
        </dl>
      </article>

      <article class="panel about-architecture">
        <div class="section-head"><div><span class="label">PROCESS MODEL</span><h3>三进程架构</h3></div></div>
        <div class="process-flow">
          <div><b>aipc-daemon</b><span>Rust · API / Web / supervisor</span></div>
          <i>→</i>
          <div><b>media_worker</b><span>C++ · ISP / VPSS / VENC</span></div>
          <i>+</i>
          <div><b>ai_worker</b><span>C++ / Lua · RKNN inference</span></div>
        </div>
      </article>
    </section>

    <section class="panel about-capabilities">
      <div class="section-head"><div><span class="label">CAPABILITIES</span><h3>当前能力</h3></div></div>
      <div class="capability-grid">
        <div><b>实时媒体</b><p>H.264、G711A、WebRTC、RTSP 和 MP4 录像。</p></div>
        <div><b>Lua 管理 AI</b><p>动态 VPSS 输入、VisionG/RKNN 模型和 last-good 回滚。</p></div>
        <div><b>标准结果接口</b><p>CloudEvents 1.0、HTTP latest、SSE replay 和目标生命周期。</p></div>
        <div><b>非阻塞 OSD</b><p>浏览器 metadata 或硬件 RGN，不让 NPU 延迟阻塞主码流。</p></div>
      </div>
    </section>

    <section class="panel about-links">
      <div class="section-head"><div><span class="label">PROJECT LINKS</span><h3>项目与依赖</h3><p>以下地址同样来自 daemon 配置，可以针对私有部署或镜像仓库覆盖。</p></div></div>
      <div class="link-grid">
        <a v-for="link in links" :key="link.label" :href="link.url" target="_blank" rel="noreferrer">
          <span>{{ link.label }}</span><b>↗</b><small>{{ link.description }}</small>
        </a>
      </div>
    </section>
  </div>
</template>
