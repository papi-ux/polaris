<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { doctorDetails } from '../doctor-details.js'
import { readConfigOrNull } from '../config-cache.js'
import { useToast } from '../composables/useToast'

// One level below Doctor's finding, step and action: the confidence, what should happen next, the
// readings behind it and the optional AI explanation. Loaded the first time it is opened.
const props = defineProps({
  doctor: { type: Object, default: null },
  stats: { type: Object, default: null },
  autoQuality: { type: Object, default: () => ({}) },
  explainable: { type: Boolean, default: false },
})
const { t } = useI18n()
const { toast: showToast } = useToast()
const view = computed(() => doctorDetails(props.doctor))

// Same AI flow Troubleshooting uses, loaded on demand so the initial bundle stays inside budget.
const explanation = ref('')
const explaining = ref(false)
async function explain() {
  if (explaining.value) return
  explaining.value = true
  explanation.value = ''
  try {
    const { explainDoctorWithAi } = await import('../ai-doctor-explanation.js')
    const configRes = await fetch('./api/config', { credentials: 'include' })
    const config = (await readConfigOrNull(configRes)) || {}
    const result = await explainDoctorWithAi({
      aiEnabled: config.ai_enabled === true || config.ai_enabled === 'enabled' || config.ai_enabled === 'true',
      config,
      supportBundle: { config, stream_stats: props.stats || {}, deterministic_summary: props.doctor || {} },
      deterministicSummary: props.doctor || {},
    })
    const answer = result.explanation || {}
    const text = [answer.likely_cause, (answer.try_first || [])[0]].filter(Boolean).join(' ')
    if (text) {
      explanation.value = text
    } else {
      showToast(result.error || t('dashboard.doctor_explain_unavailable'), 'info')
    }
  } catch (e) {
    showToast(t('dashboard.doctor_explain_unavailable') + ` (${e.message})`, 'error')
  } finally {
    explaining.value = false
  }
}
</script>

<template>
  <div class="mt-2 max-w-3xl space-y-3 text-sm leading-relaxed text-support">
    <div class="flex flex-wrap items-center gap-2" data-dashboard-doctor-meta role="list" aria-label="Doctor confidence and Live Tuning status">
      <div v-if="view.confidence" class="control-chip" role="listitem">{{ t('dashboard.doctor_confidence', { level: view.confidence }) }}</div>
      <div class="meta-pill" :class="autoQuality.toneClass" role="listitem">{{ autoQuality.compactLabel }}</div>
    </div>
    <p v-if="view.actionNote" data-doctor-action-note>{{ view.actionNote }}</p>
    <p v-if="view.expected" data-doctor-expected><span class="font-semibold text-silver">{{ t('dashboard.doctor_expected') }}: </span>{{ view.expected }}</p>
    <div v-if="view.evidence.length">
      <h4 class="font-semibold text-silver">{{ t('dashboard.doctor_evidence') }}</h4>
      <ul class="mt-1 space-y-2" data-doctor-evidence>
        <li v-for="row in view.evidence" :key="row.id">
          <span class="text-silver">{{ row.label }}</span>
          <span v-if="row.value" class="font-mono"> · {{ row.value }}</span>
          <span> · {{ t(`dashboard.doctor_status_${row.status}`) }}</span>
          <span v-if="row.detail" class="block text-xs text-storm">{{ row.detail }}</span>
        </li>
      </ul>
    </div>
    <button
      v-if="explainable"
      type="button"
      class="focus-ring dashboard-action-button dashboard-action-button-ghost disabled:cursor-wait disabled:opacity-70"
      :disabled="explaining"
      data-doctor-explain
      @click="explain"
    >
      {{ explaining ? t('dashboard.doctor_explaining') : t('dashboard.doctor_explain') }}
    </button>
    <p v-if="explanation" class="rounded-lg border border-accent/20 bg-accent/5 px-3 py-2 text-silver">{{ explanation }}</p>
  </div>
</template>
