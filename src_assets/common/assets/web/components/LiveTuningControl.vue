<script setup>
import { computed } from 'vue'
import { useLiveTuning } from '../composables/useLiveTuning'
defineProps({ compact: Boolean })
const { state, saving, error, confirmed, setEnabled, label } = useLiveTuning()
const bitrate = computed(() => state.value?.applied_bitrate_kbps > 0
  ? `${(state.value.applied_bitrate_kbps / 1000).toFixed(1)} / ${(state.value.quality_limit_kbps / 1000).toFixed(1)} Mbps limit` : '')
</script>
<template>
  <div class="rounded-xl border border-storm/20 bg-deep/35 p-3" data-live-tuning>
    <button type="button" role="switch" aria-label="Live Tuning" :aria-checked="Boolean(state?.enabled)"
      :disabled="saving || !confirmed" :aria-busy="saving"
      class="focus-ring flex w-full items-center justify-between gap-3 text-left disabled:opacity-60"
      @click="setEnabled(!state.enabled)">
      <span class="min-w-0">
        <span class="block text-sm font-medium text-silver">Live Tuning</span>
        <span class="mt-1 block text-xs text-storm" aria-live="polite">{{ saving ? 'Saving…' : label }}</span>
      </span>
      <span class="relative h-5 w-9 shrink-0 rounded-full" :class="confirmed && state?.enabled ? 'bg-accent' : 'bg-storm/40'" aria-hidden="true">
        <span class="absolute top-0.5 h-4 w-4 rounded-full bg-white transition-transform"
          :style="{ transform: state?.enabled ? 'translateX(18px)' : 'translateX(2px)' }" />
      </span>
    </button>
    <p v-if="!compact" class="mt-2 text-xs leading-relaxed text-storm">Automatically adjust stream bitrate within your quality limit. This setting applies to the host.</p>
    <p v-if="confirmed && bitrate" class="mt-2 text-xs text-silver">{{ bitrate }}<span v-if="state.pending"> · Applying requested bitrate</span></p>
    <p v-if="confirmed && state?.enabled && state?.state === 'unavailable'" class="mt-2 text-xs text-storm">{{ state.reason?.replaceAll('_', ' ') }}</p>
    <p v-if="error" role="alert" class="mt-2 text-xs text-warning-bright">{{ error }}</p>
  </div>
</template>
