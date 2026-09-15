<template>
  <details class="mt-4 border-t border-storm/20 pt-2">
    <summary class="focus-ring cursor-pointer rounded py-2 font-semibold text-silver">Desktop Access</summary>
    <p class="mt-2 text-sm text-storm">Let a device choose Desktop alongside its Spaces in Nova. This uses the host computer’s usual account, games, and files.</p>
    <label v-for="device in eligible" :key="device.uuid" class="mt-3 flex items-center gap-3 text-sm text-silver">
      <input type="checkbox" :checked="allowed.includes(device.uuid)" :disabled="locked || working"
             :aria-label="'Allow Desktop for ' + deviceName(device)" @change="save(device.uuid, $event)">
      <span>{{ deviceName(device) }}</span>
    </label>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
  </details>
</template>
<script setup>
import { computed, nextTick, ref } from 'vue'
const props = defineProps({ clients: { type: Array, default: () => [] }, allowed: { type: Array, default: () => [] },
  locked: Boolean, refresh: { type: Function, required: true } })
const emit = defineEmits(['busy'])
const working = ref(false), message = ref(''), error = ref('')
const eligible = computed(() => props.clients.filter(device => !device.temporary_authorization && (Number(device.perm) & 0x04000000) !== 0))
const deviceName = device => device.friendly_name || device.name || 'Paired Device'
async function save(client, event) {
  const requested = event.target.checked
  event.target.checked = props.allowed.includes(client)
  if (props.locked || working.value) return
  working.value = true; emit('busy', true); message.value = ''; error.value = ''
  try {
    const response = await fetch('./api/multiseat/access', { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ profile_id: 'desktop', client_id: client, allowed: requested }) })
    const result = await response.json()
    if (response.status !== 202 && (!response.ok || result.status !== true)) throw new Error(result.message || 'Could not save Desktop Access.')
    const verified = await props.refresh()
    await nextTick()
    message.value = verified && props.allowed.includes(client) === requested ? 'Desktop Access Saved.' :
      'The change has not been confirmed. Refresh Spaces to check its status.'
  } catch (cause) {
    error.value = cause.message || 'Could not save Desktop Access.'
    try { await props.refresh() } catch { /* Keep the failed request visible. */ }
  } finally { working.value = false; emit('busy', false) }
}
</script>
