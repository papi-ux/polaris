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
    <label v-if="byDefault !== undefined" class="mt-3 flex items-start gap-3 rounded-xl border border-storm/20 bg-deep/40 p-3 text-sm text-silver">
      <input type="checkbox" role="switch" class="mt-0.5 h-4 w-4 shrink-0 rounded border-storm bg-void text-ice accent-ice"
             :checked="byDefault" :disabled="locked || working" data-desktop-by-default @change="saveByDefault">
      <span class="min-w-0">
        {{ $t('spaces.desktop_by_default') }}
        <span class="mt-0.5 block text-xs text-storm">{{ $t('spaces.desktop_by_default_copy') }}</span>
      </span>
    </label>
    <p v-if="!eligible.length" class="mt-2 text-sm text-storm">{{ $t('spaces.desktop_access_empty') }}</p>
    <div v-if="eligible.length > 1" class="mt-3 flex flex-wrap items-center gap-2" data-access-bulk>
      <Button variant="ghost" size="sm" :disabled="locked || working || allowedCount === eligible.length"
              :aria-label="$t('spaces.desktop_select_all_aria')" data-access-select-all @click="setAll(true)">
        {{ $t('spaces.access_select_all') }}
      </Button>
      <Button variant="ghost" size="sm" :disabled="locked || working || !allowed.length"
              :aria-label="$t('spaces.desktop_clear_all_aria')" data-access-clear-all @click="clearOpen = true">
        {{ $t('spaces.access_clear_all') }}
      </Button>
    </div>
    <label v-for="device in eligible" :key="device.uuid" class="mt-3 flex items-center gap-3 text-sm text-silver">
      <input type="checkbox" class="h-4 w-4 shrink-0 rounded border-storm bg-void text-ice accent-ice"
             :checked="allowed.includes(device.uuid)" :disabled="locked || working"
             :aria-label="$t('spaces.desktop_allow_aria', { device: deviceName(device) })" @change="save(device.uuid, $event)">
      <span>{{ deviceName(device) }}</span>
    </label>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <ConfirmActionDialog v-model="clearOpen" :title="$t('spaces.desktop_clear_title')"
                         :message="$t('spaces.desktop_clear_message')"
                         :impact-items="[$t('spaces.desktop_clear_impact_default'), $t('spaces.desktop_clear_impact_kept')]"
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
// byDefault is the owner's "a device with a Space also gets Desktop" setting; a host from before
// 1.4.12 sends none, and the switch is then left out rather than shown off.
const props = defineProps({ clients: { type: Array, default: () => [] }, allowed: { type: Array, default: () => [] },
  byDefault: { type: Boolean, default: undefined },
  locked: Boolean, refresh: { type: Function, required: true } })
const emit = defineEmits(['busy'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const working = ref(false), message = ref(''), error = ref(''), clearOpen = ref(false)
const eligible = computed(() => props.clients.filter(device => !device.temporary_authorization && (Number(device.perm) & permissionMapping.launch) !== 0))
const nameLabels = computed(() => deviceNameLabels(props.clients, { t, fallback: t('spaces.paired_device') }))
const deviceName = device => nameLabels.value.get(device?.uuid) || device?.friendly_name || device?.name || t('spaces.paired_device')
const allowedCount = computed(() => eligible.value.filter(device => props.allowed.includes(device.uuid)).length)

// One host change for every device, where ticking them one by one restarts Spaces each time.
async function setAll(requested) {
  if (props.locked || working.value) return
  working.value = true; emit('busy', true); message.value = ''; error.value = ''
  try {
    const { pending } = await setAccessForAll('desktop', requested)
    const verified = await props.refresh()
    await nextTick()
    const done = requested ? allowedCount.value === eligible.value.length : props.allowed.length === 0
    message.value = pending || !verified || !done ? t('spaces.access_unconfirmed')
      : requested ? t('spaces.desktop_all_saved', { count: eligible.value.length }) : t('spaces.desktop_cleared')
  } catch (cause) {
    error.value = cause.unsupported ? t('spaces.access_all_unsupported') : cause.message || t('spaces.desktop_failed')
    try { await props.refresh() } catch { /* Keep the failed request visible. */ }
  } finally { working.value = false; emit('busy', false); clearOpen.value = false }
}

// The setting changes what later access changes do and nothing else, so it restarts nothing.
async function saveByDefault(event) {
  const requested = event.target.checked
  event.target.checked = props.byDefault === true
  if (props.locked || working.value) return
  working.value = true; emit('busy', true); message.value = ''; error.value = ''
  try {
    const response = await fetch('./api/multiseat/settings', { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ desktop_by_default: requested }) })
    const result = await response.json().catch(() => null)
    if (!response.ok || result?.status !== true) throw new Error(result?.message || result?.error || t('spaces.desktop_by_default_failed'))
    const verified = await props.refresh()
    await nextTick()
    message.value = verified && props.byDefault === requested
      ? t(requested ? 'spaces.desktop_by_default_on' : 'spaces.desktop_by_default_off') : t('spaces.access_unconfirmed')
  } catch (cause) {
    error.value = cause.message || t('spaces.desktop_by_default_failed')
    try { await props.refresh() } catch { /* Keep the failed request visible. */ }
  } finally { working.value = false; emit('busy', false) }
}

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
