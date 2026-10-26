import { nextTick, ref } from 'vue'

const toasts = ref([])
const immediateDismissals = ref(new Set())
let nextId = 0

export function useToast() {
  function toast(message, type = 'info', duration = 3000, action = null) {
    const id = nextId++
    toasts.value.push({ id, message, type, duration, action })
    setTimeout(() => {
      dismiss(id)
    }, duration)
    return id
  }

  function dismiss(id, { immediate = false } = {}) {
    if (immediate) {
      immediateDismissals.value = new Set([...immediateDismissals.value, id])
    }
    toasts.value = toasts.value.filter(t => t.id !== id)
    // Leave hooks run in this render flush; no dismissal state survives it.
    if (immediate) nextTick(() => immediateDismissals.value.delete(id))
  }

  return { toasts, toast, dismiss, immediateDismissals }
}
