<script setup>
import { computed, inject } from 'vue'
import { useStreamStats } from '../../../composables/useStreamStats'
import { describePyroWaveStream } from '../../../codec-support-readout.js'

// Mounted only on a host that can serve PyroWave, so a host that cannot never opens the stats stream
// from its settings page for a readout that would stay empty.
const { stats } = useStreamStats()
const i18n = inject('i18n', null)
const t = (key, params) => (i18n ? i18n.t(key, params) : key)

const stream = computed(() => describePyroWaveStream(t, stats.value))
</script>

<template>
  <div v-if="stream" class="mt-2 rounded-md border border-storm/20 p-3" data-pyrowave-stream>
    <p class="text-sm font-medium text-silver" data-pyrowave-stream-heading>{{ stream.heading }}</p>
    <p v-for="route in stream.routes" :key="route" class="mt-1 text-xs leading-relaxed text-silver" data-pyrowave-route>{{ route }}</p>
    <p v-for="line in stream.lines" :key="line" class="mt-1 text-xs leading-relaxed text-storm">{{ line }}</p>
  </div>
</template>
