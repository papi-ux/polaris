<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="nvidia-nvenc-encoder" class="config-page">
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Latency & Quality</div>
        <h3 class="settings-section-title">Primary NVENC profile</h3>
        <p class="settings-section-copy">Tune the main NVIDIA encode path for bitrate efficiency, scene-change handling, and the latency envelope you want during streaming.</p>
      </div>

      <div class="mb-3">
        <label for="nvenc_preset" class="block text-sm font-medium text-storm mb-1">{{ $t('config.nvenc_preset') }}</label>
        <select id="nvenc_preset" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.nvenc_preset">
          <option value="1">P1 {{ $t('config.nvenc_preset_1') }}</option>
          <option value="2">P2</option>
          <option value="3">P3</option>
          <option value="4">P4</option>
          <option value="5">P5</option>
          <option value="6">P6</option>
          <option value="7">P7 {{ $t('config.nvenc_preset_7') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.nvenc_preset_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="nvenc_twopass" class="block text-sm font-medium text-storm mb-1">{{ $t('config.nvenc_twopass') }}</label>
        <select id="nvenc_twopass" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.nvenc_twopass">
          <option value="disabled">{{ $t('config.nvenc_twopass_disabled') }}</option>
          <option value="quarter_res">{{ $t('config.nvenc_twopass_quarter_res') }}</option>
          <option value="full_res">{{ $t('config.nvenc_twopass_full_res') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.nvenc_twopass_desc') }}</div>
      </div>

      <Checkbox class="mb-3"
                id="nvenc_spatial_aq"
                locale-prefix="config"
                v-model="config.nvenc_spatial_aq"
                default="false"
      ></Checkbox>

      <div class="mb-3">
        <label for="nvenc_vbv_increase" class="block text-sm font-medium text-storm mb-1">{{ $t('config.nvenc_vbv_increase') }}</label>
        <input type="number" min="0" max="400" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="nvenc_vbv_increase" placeholder="0"
               v-model="config.nvenc_vbv_increase" />
        <div class="text-sm text-storm mt-1">
          {{ $t('config.nvenc_vbv_increase_desc') }}<br><br>
          <a href="https://en.wikipedia.org/wiki/Video_buffering_verifier">VBV/HRD</a>
        </div>
      </div>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Compatibility</div>
        <h3 class="settings-section-title">Driver-specific behavior</h3>
        <p class="settings-section-copy">Use these lower-level options only when a game, client, or driver stack needs more targeted behavior than the default profile.</p>
      </div>

      <Checkbox v-if="platform === 'windows'"
                class="mb-3"
                id="nvenc_realtime_hags"
                locale-prefix="config"
                v-model="config.nvenc_realtime_hags"
                default="true"
      >
        <br><br>
        <a href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/">HAGS</a>
      </Checkbox>

      <Checkbox v-if="platform === 'windows'"
                class="mb-3"
                id="nvenc_latency_over_power"
                locale-prefix="config"
                v-model="config.nvenc_latency_over_power"
                default="true"
      ></Checkbox>

      <Checkbox v-if="platform === 'windows'"
                class="mb-3"
                id="nvenc_opengl_vulkan_on_dxgi"
                locale-prefix="config"
                v-model="config.nvenc_opengl_vulkan_on_dxgi"
                default="true"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="nvenc_h264_cavlc"
                locale-prefix="config"
                v-model="config.nvenc_h264_cavlc"
                default="false"
      ></Checkbox>

      <div>
        <label for="nvenc_h264_cavlc" class="block text-sm font-medium text-storm mb-1">{{ $t('config.nvenc_intra_refresh') }}</label>
        <select id="nvenc_h264_cavlc" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.nvenc_intra_refresh">
          <option value="disabled">{{ $t('_common.auto') }}</option>
          <option value="enabled">{{ $t('_common.enabled') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.nvenc_intra_refresh_desc') }}</div>
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
