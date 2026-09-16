<template>
  <details class="settings-disclosure mt-4 border-t border-storm/20 pt-2">
    <summary class="settings-disclosure-summary focus-ring cursor-pointer rounded py-2 font-semibold text-silver">
      <span>{{ $t('spaces.desktop_access') }}</span>
      <span class="flex items-center gap-2">
        <span class="control-chip">{{ allowed.length }}</span>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7" />
        </svg>
      </span>
    </summary>
    <p class="mt-2 text-sm text-storm">{{ $t('spaces.desktop_access_copy') }}</p>
    <p v-if="!eligible.length" class="mt-2 text-sm text-storm">{{ $t('spaces.desktop_access_empty') }}</p>
    <label v-for="device in eligible" :key="device.uuid" class="mt-3 flex items-center gap-3 text-sm text-silver">
      <input type="checkbox" class="h-4 w-4 shrink-0 rounded border-storm bg-void text-ice accent-ice"
             :checked="allowed.includes(device.uuid)" :disabled="locked || working"
             :aria-label="$t('spaces.desktop_allow_aria', { device: deviceName(device) })" @change="save(device.uuid, $event)">
      <span>{{ deviceName(device) }}</span>
    </label>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
  </details>
</template>
<script setup>
import { computed, inject, nextTick, ref } from 'vue'
import { permissionMapping } from '../composables/useClients.js'
const props = defineProps({ clients: { type: Array, default: () => [] }, allowed: { type: Array, default: () => [] },
  locked: Boolean, refresh: { type: Function, required: true } })
const emit = defineEmits(['busy'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const working = ref(false), message = ref(''), error = ref('')
const eligible = computed(() => props.clients.filter(device => !device.temporary_authorization && (Number(device.perm) & permissionMapping.launch) !== 0))
const deviceName = device => device.friendly_name || device.name || t('spaces.paired_device')
async function save(client, event) {
  const requested = event.target.checked
  event.target.checked = props.allowed.includes(client)
  if (props.locked || working.value) return
  working.value = true; emit('busy', true); message.value = ''; error.value = ''
  try {
    const response = await fetch('./api/multiseat/access', { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ profile_id: 'desktop', client_id: client, allowed: requested }) })
    const result = await response.json()
    if (response.status !== 202 && (!response.ok || result.status !== true)) throw new Error(result.message || result.error || t('spaces.desktop_failed'))
    const verified = await props.refresh()
    await nextTick()
    message.value = verified && props.allowed.includes(client) === requested ? t('spaces.desktop_saved') : t('spaces.access_unconfirmed')
  } catch (cause) {
    error.value = cause.message || t('spaces.desktop_failed')
    try { await props.refresh() } catch { /* Keep the failed request visible. */ }
  } finally { working.value = false; emit('busy', false) }
}
</script>
