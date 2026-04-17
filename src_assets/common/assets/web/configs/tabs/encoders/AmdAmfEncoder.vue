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
  <div id="amd-amf-encoder" class="config-page">
    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">AMD Hardware Encoding</div>
        <h3 class="settings-section-title">AMF latency and quality</h3>
        <p class="settings-section-copy">Tune the AMD AMF path for your preferred latency target, rate-control model, and image quality behavior.</p>
      </div>

      <div class="mb-3">
        <label for="amd_usage" class="block text-sm font-medium text-storm mb-1">{{ $t('config.amd_usage') }}</label>
        <select id="amd_usage" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.amd_usage">
          <option value="transcoding">{{ $t('config.amd_usage_transcoding') }}</option>
          <option value="webcam">{{ $t('config.amd_usage_webcam') }}</option>
          <option value="lowlatency_high_quality">{{ $t('config.amd_usage_lowlatency_high_quality') }}</option>
          <option value="lowlatency">{{ $t('config.amd_usage_lowlatency') }}</option>
          <option value="ultralowlatency">{{ $t('config.amd_usage_ultralowlatency') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.amd_usage_desc') }}</div>
      </div>

      <div class="settings-subtle-surface mb-3">
        <div class="eyebrow-label mb-2">{{ $t('config.amd_rc_group') }}</div>
        <div class="mb-3">
          <label for="amd_rc" class="block text-sm font-medium text-storm mb-1">{{ $t('config.amd_rc') }}</label>
          <select id="amd_rc" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.amd_rc">
            <option value="cbr">{{ $t('config.amd_rc_cbr') }}</option>
            <option value="cqp">{{ $t('config.amd_rc_cqp') }}</option>
            <option value="vbr_latency">{{ $t('config.amd_rc_vbr_latency') }}</option>
            <option value="vbr_peak">{{ $t('config.amd_rc_vbr_peak') }}</option>
          </select>
          <div class="text-sm text-storm mt-1">{{ $t('config.amd_rc_desc') }}</div>
        </div>

        <Checkbox class="mb-0"
                  id="amd_enforce_hrd"
                  locale-prefix="config"
                  v-model="config.amd_enforce_hrd"
                  default="false"
        ></Checkbox>
      </div>

      <div class="settings-subtle-surface">
        <div class="eyebrow-label mb-2">{{ $t('config.amd_quality_group') }}</div>
        <div class="mb-3">
          <label for="amd_quality" class="block text-sm font-medium text-storm mb-1">{{ $t('config.amd_quality') }}</label>
          <select id="amd_quality" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.amd_quality">
            <option value="speed">{{ $t('config.amd_quality_speed') }}</option>
            <option value="balanced">{{ $t('config.amd_quality_balanced') }}</option>
            <option value="quality">{{ $t('config.amd_quality_quality') }}</option>
          </select>
          <div class="text-sm text-storm mt-1">{{ $t('config.amd_quality_desc') }}</div>
        </div>

        <Checkbox class="mb-3"
                  id="amd_preanalysis"
                  locale-prefix="config"
                  v-model="config.amd_preanalysis"
                  default="false"
        ></Checkbox>

        <Checkbox class="mb-3"
                  id="amd_vbaq"
                  locale-prefix="config"
                  v-model="config.amd_vbaq"
                  default="true"
        ></Checkbox>

        <div class="mb-0">
          <label for="amd_coder" class="block text-sm font-medium text-storm mb-1">{{ $t('config.amd_coder') }}</label>
          <select id="amd_coder" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.amd_coder">
            <option value="auto">{{ $t('config.ffmpeg_auto') }}</option>
            <option value="cabac">{{ $t('config.coder_cabac') }}</option>
            <option value="cavlc">{{ $t('config.coder_cavlc') }}</option>
          </select>
          <div class="text-sm text-storm mt-1">{{ $t('config.amd_coder_desc') }}</div>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
