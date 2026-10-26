import { onScopeDispose, toValue, watch } from 'vue'
import { deviceBaseName } from '../device-names.js'
import { streamClientFamilyLabel } from '../client-family.js'
import { useToast } from './useToast.js'

// Observe only the reads the current page already owns. A missing snapshot means unavailable,
// not disconnected; the first valid read on a page or after a gap is always silent.
export function useDeviceJoinedToast(source, { t, stream = false }) {
  const { toast, dismiss, toasts } = useToast()
  let previous = null
  let notices = []

  function reset() { previous = null }

  watch(() => toValue(source), snapshot => {
    if (document.hidden || snapshot == null) { reset(); return }
    let clients = snapshot
    if (stream) {
      if (typeof snapshot.streaming !== 'boolean') { reset(); return }
      clients = !snapshot.streaming ? [] : Array.isArray(snapshot.clients) && snapshot.clients.length
        ? snapshot.clients
        : [{ name: snapshot.client_name, client_family: snapshot.client_family, stream_instance_id: snapshot.stream_instance_id }]
    }
    if (!Array.isArray(clients)) { reset(); return }
    const next = new Map()
    for (const client of clients) {
      const key = stream ? client?.stream_instance_id : client?.uuid
      if (typeof key !== 'string' || !key) continue
      next.set(key, { ...client, connected: stream || client.connected === true })
    }
    if (stream && snapshot.streaming && next.size !== clients.length) { reset(); return }
    if (previous) {
      for (const [key, client] of next) {
        const before = previous.get(key)
        if (before && (before.connected || !client.connected)) continue
        const name = deviceBaseName(client)
        if (!name) continue
        // Unknown family stays unknown; a device name is never a hardware-type heuristic.
        const type = streamClientFamilyLabel(client.client_family)
        notices = notices.filter(id => toasts.value.some(notice => notice.id === id))
        notices.push(toast(t(type ? '_common.device_joined_type' : '_common.device_joined', { name, type }), 'info'))
      }
    }
    previous = next
  }, { immediate: true })

  // A hidden page may not receive any reads at all. Returning to it seeds afresh.
  document.addEventListener('visibilitychange', reset)
  onScopeDispose(() => {
    document.removeEventListener('visibilitychange', reset)
    // The page's notice must not follow navigation into the browser player or auth screens.
    notices.forEach(dismiss)
  })
}
