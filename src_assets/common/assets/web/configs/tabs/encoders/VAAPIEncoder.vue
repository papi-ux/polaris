<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";
import CodecSupportPanel from "./CodecSupportPanel.vue";

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="vaapi-encoder" class="config-page">
    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Linux GPU Encoding</div>
        <h3 class="settings-section-title">VA-API behavior</h3>
        <p class="settings-section-copy">{{ $t('config.vaapi_session_settings_desc') }}</p>
      </div>

      <CodecSupportPanel :config="config" />

      <div class="mb-3">
        <label for="vaapi_quality" class="block text-sm font-medium text-storm mb-1">{{ $t('config.vaapi_quality') }}</label>
        <select id="vaapi_quality" class="settings-input" v-model="config.vaapi_quality">
          <option value="auto">{{ $t('auto') }}</option>
          <option value="speed">{{ $t('config.vaapi_quality_speed') }}</option>
          <option value="balanced">{{ $t('config.vaapi_quality_balanced') }}</option>
          <option value="quality">{{ $t('config.vaapi_quality_quality') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.vaapi_quality_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="vaapi_rc" class="block text-sm font-medium text-storm mb-1">{{ $t('config.vaapi_rc') }}</label>
        <select id="vaapi_rc" class="settings-input" v-model="config.vaapi_rc">
          <option value="auto">{{ $t('auto') }}</option>
          <option value="cbr">CBR</option>
          <option value="vbr">VBR</option>
          <option value="avbr">AVBR</option>
          <option value="cqp">CQP</option>
          <option value="icq">ICQ</option>
          <option value="qvbr">QVBR</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.vaapi_rc_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="vaapi_blbrc" class="block text-sm font-medium text-storm mb-1">{{ $t('config.vaapi_blbrc') }}</label>
        <select id="vaapi_blbrc" class="settings-input" v-model="config.vaapi_blbrc">
          <option value="auto">{{ $t('auto') }}</option>
          <option value="enabled">{{ $t('config.vaapi_option_enabled') }}</option>
          <option value="disabled">{{ $t('config.vaapi_option_disabled') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.vaapi_blbrc_desc') }}</div>
      </div>

      <Checkbox class="mb-3"
                id="vaapi_strict_rc_buffer"
                locale-prefix="config"
                v-model="config.vaapi_strict_rc_buffer"
                default="false"
      ></Checkbox>
    </section>
  </div>
</template>

<style scoped>

</style>
