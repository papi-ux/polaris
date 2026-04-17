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
  <div id="intel-quicksync-encoder" class="config-page">
    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Intel Hardware Encoding</div>
        <h3 class="settings-section-title">QuickSync profile</h3>
        <p class="settings-section-copy">Choose the Intel preset and entropy coding strategy Polaris should use when QuickSync is the active encoder path.</p>
      </div>

      <div class="mb-3">
        <label for="qsv_preset" class="block text-sm font-medium text-storm mb-1">{{ $t('config.qsv_preset') }}</label>
        <select id="qsv_preset" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.qsv_preset">
          <option value="veryfast">{{ $t('config.qsv_preset_veryfast') }}</option>
          <option value="faster">{{ $t('config.qsv_preset_faster') }}</option>
          <option value="fast">{{ $t('config.qsv_preset_fast') }}</option>
          <option value="medium">{{ $t('config.qsv_preset_medium') }}</option>
          <option value="slow">{{ $t('config.qsv_preset_slow') }}</option>
          <option value="slower">{{ $t('config.qsv_preset_slower') }}</option>
          <option value="slowest">{{ $t('config.qsv_preset_slowest') }}</option>
        </select>
      </div>

      <div class="mb-3">
        <label for="qsv_coder" class="block text-sm font-medium text-storm mb-1">{{ $t('config.qsv_coder') }}</label>
        <select id="qsv_coder" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.qsv_coder">
          <option value="auto">{{ $t('config.ffmpeg_auto') }}</option>
          <option value="cabac">{{ $t('config.coder_cabac') }}</option>
          <option value="cavlc">{{ $t('config.coder_cavlc') }}</option>
        </select>
      </div>

      <Checkbox class="mb-3"
                id="qsv_slow_hevc"
                locale-prefix="config"
                v-model="config.qsv_slow_hevc"
                default="false"
      ></Checkbox>
    </section>
  </div>
</template>

<style scoped>

</style>
