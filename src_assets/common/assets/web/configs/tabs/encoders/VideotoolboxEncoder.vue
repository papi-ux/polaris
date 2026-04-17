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
  <div id="videotoolbox-encoder" class="config-page">
    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Apple Hardware Encoding</div>
        <h3 class="settings-section-title">VideoToolbox profile</h3>
        <p class="settings-section-copy">Define entropy coding, whether Polaris can fall back to software, and whether the encode path should stay realtime-focused.</p>
      </div>

      <div class="mb-3">
        <label for="vt_coder" class="block text-sm font-medium text-storm mb-1">{{ $t('config.vt_coder') }}</label>
        <select id="vt_coder" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.vt_coder">
          <option value="auto">{{ $t('config.ffmpeg_auto') }}</option>
          <option value="cabac">{{ $t('config.coder_cabac') }}</option>
          <option value="cavlc">{{ $t('config.coder_cavlc') }}</option>
        </select>
      </div>

      <div class="mb-3">
        <label for="vt_software" class="block text-sm font-medium text-storm mb-1">{{ $t('config.vt_software') }}</label>
        <select id="vt_software" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.vt_software">
          <option value="auto">{{ $t('_common.auto') }}</option>
          <option value="disabled">{{ $t('_common.disabled') }}</option>
          <option value="allowed">{{ $t('config.vt_software_allowed') }}</option>
          <option value="forced">{{ $t('config.vt_software_forced') }}</option>
        </select>
      </div>

      <Checkbox class="mb-0"
                id="vt_realtime"
                desc=""
                locale-prefix="config"
                v-model="config.vt_realtime"
                default="true"
      ></Checkbox>
    </section>
  </div>
</template>

<style scoped>

</style>
