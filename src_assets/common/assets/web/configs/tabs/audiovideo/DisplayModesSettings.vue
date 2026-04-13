<script setup>
import { ref } from 'vue'

const props = defineProps([
  'platform',
  'config',
])
const config = ref(props.config)
</script>

<template>
  <!--max_bitrate-->
  <div class="mb-3">
    <label for="max_bitrate" class="block text-sm font-medium text-storm mb-1">{{ $t("config.max_bitrate") }}</label>
    <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="max_bitrate" placeholder="0" v-model="config.max_bitrate" />
    <div class="text-sm text-storm mt-1">{{ $t("config.max_bitrate_desc") }}</div>
  </div>

  <!--minimum_fps_target-->
  <div class="mb-3">
    <label for="minimum_fps_target" class="block text-sm font-medium text-storm mb-1">{{ $t("config.minimum_fps_target") }}</label>
    <input type="number" min="0" max="1000" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="minimum_fps_target" placeholder="0" v-model="config.minimum_fps_target" />
    <div class="text-sm text-storm mt-1">{{ $t("config.minimum_fps_target_desc") }}</div>
  </div>
</template>

<style scoped>
.ms-item {
  background-color: #393c51;
  font-size: 12px;
  font-weight: bold;
}
</style>
