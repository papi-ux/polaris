<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
</script>

<template>
  <div class="config-page">
    <p class="-mt-3 text-right text-xs text-storm" data-tab-docs-link>
      <a href="https://papi-ux.com/docs/configuration/#advanced-tab" target="_blank" rel="noopener" class="focus-ring text-ice hover:underline">{{ $t('config.advanced_docs_link') }}</a>
    </p>
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.advanced_kicker_load_handling') }}</div>
        <h3 class="settings-section-title">{{ $t('config.advanced_section_stream_behavior_under_load') }}</h3>
      </div>

      <div class="mb-3">
        <label for="fec_percentage" class="block text-sm font-medium text-storm mb-1">{{ $t('config.fec_percentage') }}</label>
        <input type="text" class="settings-input" id="fec_percentage" placeholder="20" v-model="config.fec_percentage" />
        <div class="text-sm text-storm mt-1">{{ $t('config.fec_percentage_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="qp" class="block text-sm font-medium text-storm mb-1">{{ $t('config.qp') }}</label>
        <input type="number" class="settings-input" id="qp" placeholder="28" v-model="config.qp" />
        <div class="text-sm text-storm mt-1">{{ $t('config.qp_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="min_threads" class="block text-sm font-medium text-storm mb-1">{{ $t('config.min_threads') }}</label>
        <input type="number" class="settings-input" id="min_threads" placeholder="2" min="1" v-model="config.min_threads" />
        <div class="text-sm text-storm mt-1">{{ $t('config.min_threads_desc') }}</div>
      </div>

      <Checkbox class="mb-3"
                id="limit_framerate"
                locale-prefix="config"
                v-model="config.limit_framerate"
                default="true"
      ></Checkbox>
    </section>

    <details class="settings-section settings-section-compact settings-disclosure">
      <summary class="settings-disclosure-summary">
        <div>
          <div class="section-kicker">{{ $t('config.advanced_kicker_compatibility') }}</div>
          <h3 class="settings-section-title mt-2">{{ $t('config.advanced_section_client_and_environment_behavior') }}</h3>
          <div class="settings-summary-copy">{{ $t('config.advanced_summary_use_these_flags_only_when_you_need_legacy_behavi') }}</div>
        </div>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
      </summary>

      <div class="settings-disclosure-body">

      <Checkbox class="mb-3"
                id="envvar_compatibility_mode"
                locale-prefix="config"
                v-model="config.envvar_compatibility_mode"
                default="false"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="legacy_ordering"
                locale-prefix="config"
                v-model="config.legacy_ordering"
                default="false"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="ignore_encoder_probe_failure"
                locale-prefix="config"
                v-model="config.ignore_encoder_probe_failure"
                default="false"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="browser_streaming"
                locale-prefix="config"
                v-model="config.browser_streaming"
                default="false"
      ></Checkbox>

      <div class="mb-3">
        <label for="hevc_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.hevc_mode') }}</label>
        <select id="hevc_mode" class="settings-input" v-model="config.hevc_mode">
          <option value="0">{{ $t('config.hevc_mode_0') }}</option>
          <option value="1">{{ $t('config.hevc_mode_1') }}</option>
          <option value="2">{{ $t('config.hevc_mode_2') }}</option>
          <option value="3">{{ $t('config.hevc_mode_3') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.hevc_mode_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="av1_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.av1_mode') }}</label>
        <select id="av1_mode" class="settings-input" v-model="config.av1_mode">
          <option value="0">{{ $t('config.av1_mode_0') }}</option>
          <option value="1">{{ $t('config.av1_mode_1') }}</option>
          <option value="2">{{ $t('config.av1_mode_2') }}</option>
          <option value="3">{{ $t('config.av1_mode_3') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.av1_mode_desc') }}</div>
      </div>
      </div>
    </details>

    <details class="settings-section settings-section-compact settings-disclosure">
      <summary class="settings-disclosure-summary">
        <div>
          <div class="section-kicker">{{ $t('config.advanced_kicker_override') }}</div>
          <h3 class="settings-section-title mt-2">{{ $t('config.advanced_section_capture_and_encoder_preference') }}</h3>
          <div class="settings-summary-copy">{{ $t('config.advanced_summary_force_a_specific_capture_or_encoder_path_only_wh') }}</div>
        </div>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
      </summary>

      <div class="settings-disclosure-body">

      <div class="mb-3" v-if="platform !== 'macos'">
        <label for="capture" class="block text-sm font-medium text-storm mb-1">{{ $t('config.capture') }}</label>
        <select id="capture" class="settings-input" v-model="config.capture">
          <option value="">{{ $t('_common.autodetect') }}</option>
          <PlatformLayout :platform="platform">
            <template #linux>
              <option value="nvfbc">NvFBC</option>
              <option value="wlr">wlroots</option>
              <option value="kms">KMS</option>
              <option value="x11">X11</option>
            </template>
            <template #windows>
              <option value="ddx">Desktop Duplication API</option>
              <option value="wgc">Windows.Graphics.Capture {{ $t('_common.beta') }}</option>
            </template>
          </PlatformLayout>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.capture_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="encoder" class="block text-sm font-medium text-storm mb-1">{{ $t('config.encoder') }}</label>
        <select id="encoder" class="settings-input" v-model="config.encoder">
          <option value="">{{ $t('_common.autodetect') }}</option>
          <PlatformLayout :platform="platform">
            <template #windows>
              <option value="nvenc">NVIDIA NVENC</option>
              <option value="quicksync">Intel QuickSync</option>
              <option value="amdvce">AMD AMF/VCE</option>
            </template>
            <template #linux>
              <option value="nvenc">NVIDIA NVENC</option>
              <option value="vaapi">VA-API</option>
            </template>
            <template #macos>
              <option value="videotoolbox">VideoToolbox</option>
            </template>
          </PlatformLayout>
          <option value="software">{{ $t('config.encoder_software') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.encoder_desc') }}</div>
      </div>
      </div>
    </details>
  </div>
</template>

<style scoped>

</style>
