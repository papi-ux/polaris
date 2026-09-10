import { ref, computed, watch } from 'vue'
import { useStreamStats } from './useStreamStats'
import { createLiveTuningReducer, liveTuningLabel, parseLiveTuning } from '../live-tuning'

const state = ref(null)
const saving = ref(false)
const error = ref('')
const reduce = createLiveTuningReducer()
function accept(raw) {
  const next = reduce(raw)
  if (next) state.value = next
}
export function useLiveTuning() {
  const { stats, connected } = useStreamStats()
  watch(() => stats.value?.live_tuning, accept, { immediate: true })
  const confirmed = computed(() => connected.value && !!state.value &&
    parseLiveTuning(stats.value?.live_tuning)?.host_instance === state.value.host_instance)
  async function setEnabled(enabled) {
    if (saving.value || !confirmed.value || typeof enabled !== 'boolean') return false
    saving.value = true
    error.value = ''
    const revision = state.value.configuration_revision
    const hostInstance = state.value.host_instance
    try {
      const response = await fetch('./api/live-tuning', {
        credentials: 'include', method: 'POST',
        headers: { 'Content-Type': 'application/json', 'If-Match': `"${revision}"` },
        body: JSON.stringify({ enabled }),
      })
      const result = await response.json()
      if (state.value?.host_instance === hostInstance && result.live_tuning?.host_instance === hostInstance) accept(result.live_tuning)
      if (!response.ok || result.status !== true) {
        throw new Error(response.status === 412 ? 'Settings changed. Review the current state and try again.' : 'Live Tuning could not be saved.')
      }
      if (state.value?.host_instance !== hostInstance || !parseLiveTuning(result.live_tuning) || result.live_tuning.host_instance !== hostInstance) {
        throw new Error('Host changed. Waiting for the current host to confirm the setting.')
      }
      window.dispatchEvent(new CustomEvent('polaris:live-tuning-saved', {
        detail: { previousRevision: revision, revision: result.live_tuning.configuration_revision },
      }))
      return true
    } catch (failure) {
      error.value = failure instanceof TypeError
        ? 'Connection lost. Waiting for the host to confirm the current setting.' : failure.message
      return false
    } finally { saving.value = false }
  }
  return { state, saving, error, confirmed, setEnabled,
    label: computed(() => liveTuningLabel(state.value, confirmed.value)) }
}
