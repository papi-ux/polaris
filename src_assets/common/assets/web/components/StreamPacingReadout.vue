<script setup>
import { computed, ref, onMounted, onUnmounted } from 'vue'
import { streamPacingReadouts } from '../stream-pacing-readout.js'
const props = defineProps({ stats: { type: Object, default: () => ({}) },
  connected: { type: Boolean, default: false }, observedAt: { type: Number, default: 0 } })
const now = ref(Date.now())
let timer
onMounted(() => { timer = setInterval(() => { now.value = Date.now() }, 500) })
onUnmounted(() => clearInterval(timer))
const current = computed(() => {
  // A new receipt can arrive between timer ticks. Compare it to the current
  // clock, while retaining the timer dependency that retires silent receipts.
  const age = Math.max(now.value, Date.now()) - props.observedAt
  return props.connected && props.observedAt > 0 && age >= 0 && age <= 2000
})
const rows = computed(() => streamPacingReadouts(props.stats, current.value))
</script>

<template>
  <section class="surface-subtle p-4" data-stream-pacing-readout>
    <h3 class="text-sm font-semibold text-silver">{{ $t('config.stream_pacing_title') }}</h3>
    <p class="mt-1 text-xs leading-relaxed text-storm">{{ $t('config.stream_pacing_scope') }}</p>
    <p v-if="!rows.length" class="mt-3 text-sm text-storm">{{ $t('config.stream_pacing_unknown') }}</p>
    <article v-for="(row, index) in rows" :key="index" class="mt-3 rounded-lg border border-storm/20 p-3">
      <p class="text-sm font-medium text-silver">{{ row.client || $t('dashboard.unknown_client') }}</p>
      <dl class="mt-2 grid gap-3 sm:grid-cols-3">
        <div><dt class="text-xs text-storm">{{ $t('config.stream_pacing_request') }}</dt><dd class="mt-1 text-sm text-silver">{{ $t(`config.stream_pacing_${row.requested}`) }}</dd></div>
        <div><dt class="text-xs text-storm">{{ $t('config.stream_pacing_effective') }}</dt><dd class="mt-1 text-sm text-silver">{{ $t(`config.stream_pacing_${row.effective}`) }}</dd></div>
        <div><dt class="text-xs text-storm">{{ $t('config.stream_pacing_fallback') }}</dt><dd class="mt-1 break-words text-sm text-silver">{{ row.reason ? $t(`config.stream_pacing_${row.reasonLabel}`) : $t(row.fallbackKnown ? 'config.stream_pacing_none' : 'config.stream_pacing_unknown') }}</dd></div>
      </dl>
      <details v-if="row.reason" class="mt-2 text-xs text-storm"><summary class="cursor-pointer">{{ $t('config.stream_pacing_fallback_detail') }}</summary><p class="mt-1 break-words font-mono">{{ row.reason }}</p></details>
    </article>
  </section>
</template>
