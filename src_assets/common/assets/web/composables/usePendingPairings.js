import { onScopeDispose, ref, watch } from 'vue'

export function usePendingPairings(active) {
  const pendingPairings = ref([])
  const selectedPairingId = ref('')
  const pairingListError = ref('')
  let timer = null
  let request = null
  let disposed = false

  async function refreshPairings() {
    if (disposed || request) return
    const controller = new AbortController()
    request = controller
    try {
      const response = await fetch('./api/pin', { credentials: 'include', signal: controller.signal })
      if (!response.ok) throw new Error('Could not load pending pairing requests.')
      const body = await response.json()
      if (!Array.isArray(body.pairings)) throw new Error('Could not load pending pairing requests.')
      if (disposed || controller.signal.aborted) return
      pendingPairings.value = body.pairings
      pairingListError.value = ''
      if (!body.pairings.some((pairing) => pairing.id === selectedPairingId.value)) {
        selectedPairingId.value = ''
      }
    } catch (error) {
      if (disposed || controller.signal.aborted) return
      pendingPairings.value = []
      selectedPairingId.value = ''
      pairingListError.value = error.message
    } finally {
      if (request === controller) request = null
    }
  }

  watch(active, (enabled) => {
    clearInterval(timer)
    request?.abort()
    request = null
    pendingPairings.value = []
    selectedPairingId.value = ''
    if (enabled) {
      refreshPairings()
      timer = setInterval(refreshPairings, 3000)
    }
  }, { immediate: true })

  onScopeDispose(() => {
    disposed = true
    clearInterval(timer)
    request?.abort()
  })

  return { pendingPairings, selectedPairingId, pairingListError, refreshPairings }
}
