<script setup>
import { ref } from 'vue'

// One disclosure for Mission Control's dense detail. The dashboard owns whether it is open, so a
// telemetry update or a change of stage never resets the viewer's choice, and nothing in here
// takes focus on its own.
defineProps({
  modelValue: { type: Boolean, default: false },
  title: { type: String, required: true },
  summary: { type: String, default: '' },
})
defineEmits(['update:modelValue'])
const uid = Math.random().toString(36).slice(2)
const titleId = `mission-diagnostics-title-${uid}`
const summaryId = `mission-diagnostics-summary-${uid}`
const bodyId = `mission-diagnostics-body-${uid}`
const toggle = ref(null)
defineExpose({ toggle })
</script>

<template>
  <section class="mc-diag" data-mission-diagnostics>
    <h3 class="text-base font-normal">
      <button ref="toggle" type="button" class="focus-ring mc-diag-toggle" :aria-expanded="modelValue ? 'true' : 'false'" :aria-controls="bodyId"
        :aria-labelledby="titleId" :aria-describedby="summary ? summaryId : undefined" data-mission-diagnostics-toggle
        @click="$emit('update:modelValue', !modelValue)">
        <span :id="titleId" class="mc-diag-title">{{ title }}
          <svg class="mc-diag-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true" focusable="false"><path d="M6 9l6 6 6-6" /></svg>
        </span>
        <span v-if="summary" :id="summaryId" class="mc-diag-summary">{{ summary }}</span>
      </button>
    </h3>
    <div v-show="modelValue" :id="bodyId" class="mc-diag-body" role="region" :aria-labelledby="titleId" data-mission-diagnostics-body>
      <slot />
    </div>
  </section>
</template>
