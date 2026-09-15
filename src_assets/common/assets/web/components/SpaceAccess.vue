<template>
  <details class="mt-4 border-t border-storm/20 pt-2">
    <summary class="focus-ring cursor-pointer rounded py-2 text-sm text-ice">Device Access</summary>
    <p class="mt-2 text-xs text-storm">Allow paired devices to choose this Space in Nova. One device can play here at a time. Stop Space streams before changing access.</p>
    <p v-if="!devices.length" class="mt-2 text-sm text-storm">Pair a device with permission to launch apps first.</p>
    <label v-for="device in devices" :key="device.uuid" class="mt-3 flex items-center gap-3 text-sm text-silver">
      <input type="checkbox" :checked="allowed(device.uuid)" :disabled="locked || working || !ready || isDefault(device.uuid)"
             :aria-label="'Allow ' + deviceName(device) + ' to use ' + space.name" @change="save(device.uuid, $event)">
      <span class="min-w-0 break-words">{{ deviceName(device) }}<span v-if="isDefault(device.uuid)" class="block text-xs text-storm">Default Space</span></span>
    </label>
    <p class="mt-3 text-xs text-storm">To remove a device’s default access, change its Default Space below.</p>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
  </details>
</template>
<script setup>
import { computed, nextTick, ref } from 'vue'
const props = defineProps({ space: { type: Object, required: true }, clients: { type: Array, default: () => [] },
  locked: Boolean, ready: Boolean, refresh: { type: Function, required: true } })
const emit = defineEmits(['busy'])
const working = ref(false), error = ref(''), message = ref('')
const devices = computed(() => props.clients.filter(client => !client.temporary_authorization && (Number(client.perm) & 0x04000000) !== 0))
const deviceName = device => device.friendly_name || device.name || 'Paired Device'
const isDefault = id => props.space.clients.includes(id)
const allowed = id => isDefault(id) || (props.space.access_clients || []).includes(id)
async function save(client, event) {
  const requested = event.target.checked
  event.target.checked = allowed(client)
  if (props.locked || working.value || !props.ready || isDefault(client)) return
  working.value = true; emit('busy', true); error.value = ''; message.value = ''
  try {
    const response = await fetch('./api/multiseat/access', { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ profile_id: props.space.id, client_id: client, allowed: requested }) })
    const result = await response.json()
    if (response.status !== 202 && (!response.ok || result.status !== true)) throw new Error(result.message || 'Could not save Device Access.')
    const verified = await props.refresh()
    await nextTick()
    message.value = verified && props.ready && allowed(client) === requested ? 'Device Access Saved.' :
      'The change has not been confirmed. Refresh Spaces to check its status.'
  } catch (cause) {
    error.value = cause.message || 'Could not save Device Access.'
    try { await props.refresh() } catch { /* Keep the failed request visible. */ }
  } finally { working.value = false; emit('busy', false) }
}
</script>
