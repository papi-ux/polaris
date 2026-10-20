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
  <div class="mt-4 border-t border-storm/20 pt-3" data-pyrowave-stream>
    <!-- No stats yet is unknown, not idle: say we are waiting rather than that nothing runs. -->
    <p v-if="!stream" class="text-sm leading-relaxed text-support" data-pyrowave-stream-state="unknown">
      {{ $t('config.pyrowave_card_stream_waiting') }}
    </p>
    <p v-else-if="stream.idle" class="text-sm leading-relaxed text-support" data-pyrowave-stream-state="idle">
      {{ $t('config.pyrowave_card_stream_idle') }}
    </p>
    <template v-else>
      <p class="text-sm font-medium text-silver" data-pyrowave-stream-heading :data-pyrowave-stream-state="stream.running ? 'running' : 'last'">
        {{ stream.heading }}
      </p>
      <dl class="mt-3 grid gap-x-5 gap-y-3 sm:grid-cols-2 xl:grid-cols-3">
        <div v-for="item in stream.values" :key="item.label" class="grid grid-cols-2 gap-3 sm:block">
          <dt class="text-sm text-support">{{ item.label }}</dt>
          <dd class="break-words text-sm font-medium text-silver sm:mt-1" :data-pyrowave-value="item.id">{{ item.value }}</dd>
        </div>
      </dl>
      <p v-for="line in stream.lines" :key="line" class="mt-1 text-sm leading-relaxed text-support">{{ line }}</p>
      <details v-if="stream.details.length" class="mt-2" data-pyrowave-stream-details>
        <summary class="focus-ring cursor-pointer text-sm text-ice">{{ $t('config.pyrowave_card_stream_details') }}</summary>
        <p v-for="line in stream.details" :key="line" class="mt-1 text-sm leading-relaxed text-support">{{ line }}</p>
      </details>
    </template>
  </div>
</template>
