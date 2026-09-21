<template>
  <details class="settings-disclosure mt-4 border-t border-storm/20 pt-2">
    <summary class="settings-disclosure-summary focus-ring cursor-pointer rounded py-2 text-sm text-ice">
      <span>{{ $t('spaces.device_access') }}</span>
      <span class="flex items-center gap-2">
        <span class="control-chip">{{ allowedCount }}</span>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7" />
        </svg>
      </span>
    </summary>
    <p class="mt-2 text-xs text-storm">{{ $t('spaces.device_access_copy') }}</p>
    <p v-if="!devices.length" class="mt-2 text-sm text-storm">{{ $t('spaces.device_access_empty') }}</p>
    <div v-if="devices.length > 1" class="mt-2 flex flex-wrap items-center gap-2" data-access-bulk>
      <Button variant="ghost" size="sm" :disabled="blocked || allowedCount === devices.length"
              :aria-label="$t('spaces.access_select_all_aria', { space: space.name })"
              :aria-describedby="lockReasonId || undefined" data-access-select-all @click="setAll(true)">
        {{ $t('spaces.access_select_all') }}
      </Button>
      <Button variant="ghost" size="sm" :disabled="blocked || !listedCount"
              :aria-label="$t('spaces.access_clear_all_aria', { space: space.name })"
              :aria-describedby="lockReasonId || undefined" data-access-clear-all @click="clearOpen = true">
        {{ $t('spaces.access_clear_all') }}
      </Button>
    </div>
    <label v-for="device in devices" :key="device.uuid" class="mt-3 flex items-center gap-3 text-sm text-silver">
      <input type="checkbox" class="h-4 w-4 shrink-0 rounded border-storm bg-void text-ice accent-ice"
             :checked="allowed(device.uuid)" :disabled="locked || working || !ready"
             :aria-label="$t('spaces.allow_aria', { device: deviceName(device), space: space.name })"
             :aria-describedby="lockReasonId || undefined" @change="save(device.uuid, $event)">
      <span class="min-w-0 break-words">
        {{ deviceName(device) }}
        <span v-if="isDefault(device.uuid)" class="block text-xs text-storm">
          {{ $t('spaces.default_space') }}
          <button type="button" class="focus-ring ml-1 rounded text-ice hover:underline" data-default-change @click.prevent="emit('open-default')">{{ $t('spaces.default_change') }}</button>
        </span>
      </span>
    </label>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <ConfirmActionDialog v-model="clearOpen" :title="$t('spaces.access_clear_title', { space: space.name })"
                         :message="$t('spaces.access_clear_message')"
                         :impact-items="[$t('spaces.access_clear_impact_default'), $t('spaces.access_clear_impact_kept')]"
                         :confirm-label="$t('spaces.access_clear_all')" :cancel-label="$t('spaces.cancel')"
                         :pending-label="$t('spaces.saving')" :pending="working"
                         :eyebrow="$t('spaces.kicker')" :impact-label="$t('spaces.dialog_impact')"
                         @confirm="setAll(false)" />
  </details>
</template>
<script setup>
import { computed, inject, nextTick, ref } from 'vue'
import Button from './Button.vue'
import ConfirmActionDialog from './ConfirmActionDialog.vue'
import { permissionMapping } from '../composables/useClients.js'
import { deviceNameLabels } from '../device-names.js'
import { setAccessForAll } from '../spaces-bulk-access.js'
const props = defineProps({ space: { type: Object, required: true }, clients: { type: Array, default: () => [] },
  locked: Boolean, ready: Boolean, lockReasonId: { type: String, default: '' }, refresh: { type: Function, required: true } })
const emit = defineEmits(['busy', 'open-default'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const working = ref(false), error = ref(''), message = ref(''), clearOpen = ref(false)
const devices = computed(() => props.clients.filter(client => !client.temporary_authorization && (Number(client.perm) & permissionMapping.launch) !== 0))
const nameLabels = computed(() => deviceNameLabels(props.clients, { t, fallback: t('spaces.paired_device') }))
const deviceName = device => nameLabels.value.get(device?.uuid) || device?.friendly_name || device?.name || t('spaces.paired_device')
const isDefault = id => props.space.clients.includes(id)
const allowed = id => isDefault(id) || (props.space.access_clients || []).includes(id)
const allowedCount = computed(() => devices.value.filter(device => allowed(device.uuid)).length)
// Everything the host lists for this Space, a device that was unpaired since included: clear all
// empties that too, so it is offered while anything at all is listed.
const listedCount = computed(() => new Set([...(props.space.clients || []), ...(props.space.access_clients || [])]).size)
const blocked = computed(() => props.locked || working.value || !props.ready)

// One host change for every device, where ticking them one by one restarts Spaces each time.
async function setAll(requested) {
  if (blocked.value) return
  working.value = true; emit('busy', true); error.value = ''; message.value = ''
  try {
    const { pending } = await setAccessForAll(props.space.id, requested)
    const verified = await props.refresh()
    await nextTick()
    const done = requested ? allowedCount.value === devices.value.length : listedCount.value === 0
    message.value = pending || !verified || !props.ready || !done ? t('spaces.access_unconfirmed')
      : requested ? t('spaces.access_all_saved', { count: devices.value.length, space: props.space.name })
      : t('spaces.access_cleared', { space: props.space.name })
  } catch (cause) {
    error.value = cause.unsupported ? t('spaces.access_all_unsupported') : cause.message || t('spaces.access_failed')
    try { await props.refresh() } catch { /* Keep the failed request visible. */ }
  } finally { working.value = false; emit('busy', false); clearOpen.value = false }
}
async function save(client, event) {
  const requested = event.target.checked
  event.target.checked = allowed(client)
  if (props.locked || working.value || !props.ready) return
  working.value = true; emit('busy', true); error.value = ''; message.value = ''
  try {
    const response = await fetch('./api/multiseat/access', { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ profile_id: props.space.id, client_id: client, allowed: requested }) })
    const result = await response.json()
    if (response.status !== 202 && (!response.ok || result.status !== true)) throw new Error(result.message || result.error || t('spaces.access_failed'))
    const verified = await props.refresh()
    await nextTick()
    message.value = verified && props.ready && allowed(client) === requested ? t('spaces.access_saved') : t('spaces.access_unconfirmed')
  } catch (cause) {
    error.value = cause.message || t('spaces.access_failed')
    try { await props.refresh() } catch { /* Keep the failed request visible. */ }
  } finally { working.value = false; emit('busy', false) }
}
</script>
