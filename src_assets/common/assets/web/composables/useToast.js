import { ref } from 'vue'

const toasts = ref([])
let nextId = 0

export function useToast() {
  function toast(message, type = 'info', duration = 3000, action = null) {
    const id = nextId++
    toasts.value.push({ id, message, type, duration, action })
    setTimeout(() => {
      dismiss(id)
    }, duration)
  }

  function dismiss(id) {
    toasts.value = toasts.value.filter(t => t.id !== id)
  }

  return { toasts, toast, dismiss }
}
