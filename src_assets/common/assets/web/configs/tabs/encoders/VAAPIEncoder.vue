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
  <div id="vaapi-encoder" class="config-page">
    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Linux GPU Encoding</div>
        <h3 class="settings-section-title">VA-API behavior</h3>
        <p class="settings-section-copy">Use this when you need stricter rate-control compliance on AMD or Intel Linux encode paths.</p>
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
