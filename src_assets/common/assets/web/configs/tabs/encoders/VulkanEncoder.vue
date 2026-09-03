<script setup>
import { ref } from 'vue'

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="vulkan-encoder" class="config-page">
    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Linux GPU Encoding</div>
        <h3 class="settings-section-title">Vulkan Video behavior</h3>
        <p class="settings-section-copy">Tune Polaris's experimental Vulkan Video path for low-latency hardware encoding.</p>
      </div>

      <div class="surface-subtle mb-4 p-4 text-sm leading-relaxed text-storm">
        Auto can prefer Vulkan Video on a compatible AMD private-stream route after Polaris verifies the exact live GPU-native frame path. NVIDIA's proprietary driver remains on NVENC, Nouveau uses capability probing, and Intel remains on VA-API by default.
        Explicit Vulkan selection is strict and supports DRM/KMS, wlroots, and Portal capture. Portal and retired DMA-BUF routes use the Vulkan RAM uploader rather than silently changing encoders.
        H.264 and HEVC are enabled; AV1 remains unavailable until the bundled FFmpeg path passes Vulkan validation. Doctor reports the detected driver, selection policy, fallback state, and reason.
      </div>

      <div class="mb-3">
        <label for="vk_tune" class="block text-sm font-medium text-storm mb-1">{{ $t('config.vk_tune') }}</label>
        <select id="vk_tune" class="settings-input" v-model="config.vk_tune">
          <option value="0">{{ $t('config.ffmpeg_auto') }}</option>
          <option value="1">{{ $t('config.vk_tune_hq') }}</option>
          <option value="2">{{ $t('config.vk_tune_ll') }}</option>
          <option value="3">{{ $t('config.vk_tune_ull') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.vk_tune_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="vk_rc_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.vk_rc_mode') }}</label>
        <select id="vk_rc_mode" class="settings-input" v-model="config.vk_rc_mode">
          <option value="0">{{ $t('config.ffmpeg_auto') }}</option>
          <option value="1">{{ $t('config.vk_rc_cqp') }}</option>
          <option value="2">{{ $t('config.vk_rc_cbr') }}</option>
          <option value="4">{{ $t('config.vk_rc_vbr') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.vk_rc_mode_desc') }}</div>
      </div>
    </section>
  </div>
</template>

<style scoped>
</style>
