<template>
  <div>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <div class="mt-4 grid gap-3 md:grid-cols-2 xl:grid-cols-3">
      <article v-for="space in active" :key="space.id" class="min-w-0 rounded-xl border border-storm/20 bg-deep/40 p-4" :data-space="space.id">
        <div class="flex items-start gap-3">
          <span class="flex h-10 w-10 shrink-0 items-center justify-center rounded-full bg-ice/10 font-semibold text-ice" aria-hidden="true">{{ initials(space.name) }}</span>
          <div class="min-w-0 flex-1">
            <h3 class="break-words font-semibold text-silver">{{ space.name }}</h3>
            <div class="mt-1 flex flex-wrap items-center gap-2">
              <StatusBadge :status="statusTone(space)" :label="activitySummary(space)" role="status" />
              <span v-if="launcherName(space)" class="control-chip" data-space-launcher>{{ launcherName(space) }}</span>
            </div>
          </div>
        </div>
        <p class="mt-3 break-words text-sm text-storm" :title="deviceNames(space).join(', ')" data-space-devices>{{ deviceSummary(space) }}</p>
        <SpaceRuntimeMove :space="space" :job="runtimeMoveJob" :available="runtimeMoveAvailable" :locked="locked"
                          :lock-reason-id="lockReasonId" :ready="ready" :refresh="refresh" @busy="emit('busy', $event)" />
        <div v-if="manageable" class="mt-4 flex flex-wrap gap-2">
          <Button variant="outline" size="sm" :disabled="locked" :aria-label="$t('spaces.rename_aria', { name: space.name })"
                  :aria-describedby="lockReasonId || undefined" @click="openRename(space)">{{ $t('spaces.rename') }}</Button>
          <Button variant="ghost" size="sm" class="text-warning-bright hover:text-warning-bright" :disabled="locked"
                  :aria-label="$t('spaces.remove_aria', { name: space.name })" :aria-describedby="lockReasonId || undefined"
                  @click="openDialog(space, 'remove')">{{ $t('spaces.remove') }}</Button>
        </div>
        <form v-if="renaming?.id === space.id" ref="renamePanel" tabindex="-1" class="mt-4 rounded-xl border border-ice/30 bg-deep p-4"
              :aria-label="$t('spaces.rename_title', { name: renaming.name })" @submit.prevent="submitRename">
          <label for="space-edit-name" class="block text-sm text-silver">{{ $t('spaces.space_name') }}</label>
          <input id="space-edit-name" v-model="name" maxlength="128" autocomplete="off" :disabled="working"
                 class="settings-input mt-2 text-sm">
          <p class="mt-2 text-xs text-storm">{{ $t('spaces.rename_help') }}</p>
          <div class="mt-4 flex flex-wrap gap-2">
            <Button type="submit" variant="outline" size="sm" :loading="working" :disabled="locked || working || !validName">
              {{ working ? $t('spaces.saving') : $t('spaces.save_name') }}
            </Button>
            <Button type="button" variant="ghost" size="sm" :disabled="working" @click="closeRename">
              {{ submitted ? $t('spaces.close') : $t('spaces.cancel') }}
            </Button>
          </div>
        </form>
        <SpaceAccess v-if="accessAvailable" :space="space" :clients="clients" :locked="locked" :lock-reason-id="lockReasonId"
                     :ready="ready" :refresh="refresh" @busy="emit('busy', $event)" @open-default="emit('open-default')" />
      </article>
    </div>
    <p v-if="!active.length" class="mt-3 text-sm text-storm" data-spaces-empty>
      {{ creationAvailable ? $t('spaces.no_spaces_create') : $t('spaces.no_spaces_setup') }}
      <span v-if="removed.length">{{ $t('spaces.or_restore') }}</span>
    </p>
    <details v-if="removed.length" class="settings-disclosure mt-4 text-sm text-storm">
      <summary class="settings-disclosure-summary focus-ring cursor-pointer rounded py-2">
        <span>{{ $t('spaces.archived') }}</span>
        <span class="flex items-center gap-2">
          <span class="control-chip">{{ removed.length }}</span>
          <svg class="settings-disclosure-chevron h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7" />
          </svg>
        </span>
      </summary>
      <p class="mt-2">{{ $t('spaces.archived_copy') }}</p>
      <div v-for="space in removed" :key="space.id" class="mt-3 flex flex-wrap items-center justify-between gap-2 rounded-lg border border-storm/20 p-3">
        <span class="min-w-0 break-words">{{ space.name }}</span>
        <div v-if="manageable" class="flex flex-wrap gap-2">
          <Button variant="ghost" size="sm" class="text-ice" :disabled="locked"
                  :aria-label="$t('spaces.restore_aria', { name: space.name })" @click="openDialog(space, 'restore')">{{ $t('spaces.restore') }}</Button>
          <Button v-if="removalAvailable" variant="ghost" size="sm" class="text-warning-bright hover:text-warning-bright" :disabled="locked"
                  :aria-label="$t('spaces.delete_aria', { name: space.name })" :aria-describedby="lockReasonId || undefined"
                  @click="openDialog(space, 'delete')">{{ $t('spaces.delete_confirm') }}</Button>
        </div>
      </div>
    </details>
    <ConfirmActionDialog v-model="dialogOpen" :title="dialogTitle" :message="dialogMessage" :impact-items="dialogImpact"
                         :confirm-label="dialogConfirmLabel" :cancel-label="$t('spaces.cancel')"
                         :pending-label="removingForGood ? $t('spaces.deleting') : $t('spaces.saving')"
                         :pending="working" :confirm-disabled="removingForGood && !removalReady" :error="dialogError"
                         :eyebrow="$t('spaces.kicker')" :impact-label="$t('spaces.dialog_impact')"
                         @confirm="confirmDialog" @cancel="cancelDialog">
      <fieldset v-if="offersChoice" class="mt-4 space-y-2" data-remove-choice>
        <legend class="text-sm font-semibold text-silver">{{ $t('spaces.remove_choice') }}</legend>
        <label class="flex cursor-pointer items-start gap-3 rounded-xl border border-storm/20 p-3 text-sm">
          <input v-model="removeMode" type="radio" name="space-remove-mode" value="archive" :disabled="working"
                 class="mt-0.5 h-4 w-4 shrink-0 border-storm bg-void text-ice accent-ice" data-remove-archive>
          <span class="min-w-0">
            <span class="block font-medium text-silver">{{ $t('spaces.remove_archive') }}</span>
            <span class="block text-storm">{{ $t('spaces.remove_archive_help') }}</span>
          </span>
        </label>
        <label class="flex items-start gap-3 rounded-xl border border-storm/20 p-3 text-sm" :class="lastSpace ? 'cursor-not-allowed' : 'cursor-pointer'">
          <input v-model="removeMode" type="radio" name="space-remove-mode" value="delete" :disabled="working || lastSpace"
                 :aria-describedby="lastSpace ? 'space-remove-last' : undefined"
                 class="mt-0.5 h-4 w-4 shrink-0 border-storm bg-void text-ice accent-ice" data-remove-delete>
          <span class="min-w-0">
            <span class="block font-medium text-warning-bright">{{ $t('spaces.remove_delete') }}</span>
            <span class="block text-storm">{{ $t('spaces.remove_delete_help') }}</span>
          </span>
        </label>
      </fieldset>
      <p v-if="lastSpace && (offersChoice || removingForGood)" id="space-remove-last" class="mt-3 text-sm text-warning-bright" data-remove-last>
        {{ $t('spaces.delete_last_space') }}
      </p>
      <div v-if="removingForGood && !lastSpace" class="mt-4">
        <label for="space-remove-name" class="block text-sm text-silver">{{ $t('spaces.delete_type_label', { name: dialogSpace?.name }) }}</label>
        <input id="space-remove-name" v-model="typedName" type="text" autocomplete="off" spellcheck="false" maxlength="128"
               :disabled="working" aria-describedby="space-remove-name-help" class="settings-input mt-2 text-sm" data-remove-name>
        <p id="space-remove-name-help" class="mt-1 text-xs text-storm">{{ $t('spaces.delete_type_help') }}</p>
      </div>
    </ConfirmActionDialog>
  </div>
</template>

<script setup>
import { computed, inject, nextTick, ref, watch } from 'vue'
import Button from './Button.vue'
import ConfirmActionDialog from './ConfirmActionDialog.vue'
import SpaceAccess from './SpaceAccess.vue'
import SpaceRuntimeMove from './SpaceRuntimeMove.vue'
import StatusBadge from './StatusBadge.vue'
import { permissionMapping } from '../composables/useClients.js'
import { deviceNameLabels } from '../device-names.js'
import { useToast } from '../composables/useToast.js'

const props = defineProps({ profiles: { type: Array, default: () => [] }, clients: { type: Array, default: () => [] },
  activity: { type: Array, default: null }, refreshing: Boolean,
  accessAvailable: Boolean, creationAvailable: Boolean, manageable: Boolean, removalAvailable: Boolean, locked: Boolean, ready: Boolean,
  runtimeMoveAvailable: Boolean, runtimeMoveJob: { type: Object, default: null },
  lockReasonId: { type: String, default: '' }, refresh: { type: Function, required: true } })
const emit = defineEmits(['busy', 'open-default'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const { toast } = useToast()
const active = computed(() => props.profiles.filter(space => !space.archived))
const removed = computed(() => props.profiles.filter(space => space.archived))
const renaming = ref(null), name = ref(''), working = ref(false), message = ref(''), error = ref(''), renamePanel = ref(null)
const dialogOpen = ref(false), dialogSpace = ref(null), dialogOperation = ref(''), dialogError = ref('')
const removeMode = ref('archive'), typedName = ref(''), requestId = ref('')
let opener = null
const pending = ref(null), submitted = ref(false)
const validName = computed(() => !!name.value.trim() && new TextEncoder().encode(name.value.trim()).length <= 128 && !/[\u0000-\u001f\u007f]/u.test(name.value))
const canLaunch = client => client && !client.temporary_authorization && (Number(client.perm) & permissionMapping.launch) !== 0
const nameLabels = computed(() => deviceNameLabels(props.clients, { t, fallback: t('spaces.paired_device') }))
const deviceName = device => nameLabels.value.get(device?.uuid) || device?.friendly_name || device?.name || t('spaces.paired_device')

// Remove on a card offers a choice only when the host can remove for good.
// Archive stays selected: deleting games and saves is never the default.
const offersChoice = computed(() => dialogOperation.value === 'remove' && props.removalAvailable)
const removingForGood = computed(() => dialogOperation.value === 'delete' || (offersChoice.value && removeMode.value === 'delete'))
// A new Space is made from an existing one of the same launcher family, so the
// host keeps the last Space of each family rather than the last Space overall.
const lastSpace = computed(() => {
  const family = dialogSpace.value?.family
  return !!family && !props.profiles.some(space => space.id !== dialogSpace.value.id && space.family === family)
})
// The typed name has to be the Space's name exactly, as the host checks it.
const removalReady = computed(() => !!dialogSpace.value && !lastSpace.value && typedName.value === dialogSpace.value.name)

// The same filter Device Access uses: a device that lost launch permission
// is not listed as able to open the Space.
function deviceNames(space) {
  return [...new Set([...space.clients, ...(space.access_clients || [])])]
    .map(id => props.clients.find(client => client.uuid === id))
    .filter(device => device && (canLaunch(device) || space.clients.includes(device.uuid)))
    .map(deviceName)
}
// Two names and a count. A Space open to a dozen devices used to spell out all twelve here, and
// then list them again under Device Access; the full list is a hover away and one click below.
function deviceSummary(space) {
  const names = deviceNames(space)
  if (!names.length) return t('spaces.no_devices')
  if (names.length <= 2) return t('spaces.available_to', { devices: names.join(', ') })
  return t('spaces.available_to_more', { devices: names.slice(0, 2).join(', '), count: names.length - 2 })
}
// Product names, so they are not translated. A family this console does not know says nothing.
const launcherNames = { steam: 'Steam', heroic: 'Heroic', lutris: 'Lutris' }
function launcherName(space) { return launcherNames[space.family] || '' }
function initials(value) { return value.trim().split(/\s+/u).slice(0, 2).map(word => [...word][0] || '').join('').toLocaleUpperCase() }
function spaceActivity(space) { return (props.activity || []).filter(item => item.profile_id === space.id) }
function activitySummary(space) {
  if (!props.activity) return props.refreshing ? t('spaces.status_checking') : t('spaces.status_unknown')
  const activity = spaceActivity(space)
  // A Space made for another NVIDIA driver cannot start until it moves.
  if (!activity.length) return props.ready && !space.runtime_mismatch ? t('spaces.status_ready') : t('spaces.status_attention')
  return activity.map(item => {
    const device = deviceName(props.clients.find(client => client.uuid === item.client_id))
    return t(item.state === 'running' ? 'spaces.status_playing' : item.state === 'starting' ? 'spaces.status_starting' : 'spaces.status_stopping', { device })
  }).join(' · ')
}
function statusTone(space) {
  if (!props.activity) return 'warning'
  const activity = spaceActivity(space)
  if (!activity.length) return props.ready && !space.runtime_mismatch ? 'pass' : 'warning'
  return activity.every(item => item.state === 'running') ? 'pass' : 'warning'
}

const dialogTitle = computed(() => {
  const space = dialogSpace.value
  if (!space) return ''
  if (removingForGood.value) return t('spaces.delete_title', { name: space.name })
  return t(dialogOperation.value === 'restore' ? 'spaces.restore_title' : 'spaces.remove_title', { name: space.name })
})
const dialogMessage = computed(() => {
  const space = dialogSpace.value
  if (!space) return ''
  if (removingForGood.value) return t('spaces.delete_message', { name: space.name })
  return t(dialogOperation.value === 'restore' ? 'spaces.restore_message' : 'spaces.remove_message', { name: space.name })
})
const dialogImpact = computed(() => {
  if (dialogOperation.value === 'restore') return [t('spaces.restore_impact_devices'), t('spaces.restore_impact_streams')]
  if (removingForGood.value) {
    return [t('spaces.delete_impact_data'), t('spaces.delete_impact_undo'),
      ...(dialogSpace.value?.archived ? [] : [t('spaces.delete_impact_devices')]), t('spaces.remove_impact_streams')]
  }
  return [t('spaces.remove_impact_kept'), t('spaces.remove_impact_restore'), t('spaces.remove_impact_disk'), t('spaces.remove_impact_streams')]
})
const dialogConfirmLabel = computed(() => dialogOperation.value === 'restore' ? t('spaces.restore') :
  removingForGood.value ? t('spaces.delete_confirm') : offersChoice.value ? t('spaces.archive_confirm') : t('spaces.remove'))

// The host takes a request identity so a retry of the same removal is confirmed.
function newRequestId() {
  if (typeof globalThis.crypto?.randomUUID === 'function') return globalThis.crypto.randomUUID()
  const bytes = globalThis.crypto.getRandomValues(new Uint8Array(16))
  bytes[6] = (bytes[6] & 0x0f) | 0x40
  bytes[8] = (bytes[8] & 0x3f) | 0x80
  const hex = [...bytes].map(byte => byte.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}
// The host's reason and its fix, and any Docker resource a removal could not delete.
function hostSentence(result) {
  const parts = [result?.message || result?.error, result?.action]
  if (result?.kept_volume) parts.push(t('spaces.delete_kept_volume', { volume: result.kept_volume }))
  if (result?.kept_network) parts.push(t('spaces.delete_kept_network', { network: result.kept_network }))
  return parts.filter(part => typeof part === 'string' && part).join(' ')
}

async function openRename(space) {
  if (props.locked || !props.manageable) return
  opener = document.activeElement
  renaming.value = { ...space }; name.value = space.name
  error.value = ''; message.value = ''; submitted.value = false; pending.value = null
  await nextTick(); renamePanel.value?.focus?.()
}
async function closeRename() { renaming.value = null; await nextTick(); if (opener?.isConnected) opener.focus() }
function resetDialog() { dialogSpace.value = null; dialogOperation.value = ''; removeMode.value = 'archive'; typedName.value = '' }
function openDialog(space, operation) {
  if (props.locked || !props.manageable) return
  resetDialog()
  dialogSpace.value = { ...space }; dialogOperation.value = operation; dialogError.value = ''
  requestId.value = operation === 'restore' ? '' : newRequestId()
  error.value = ''; message.value = ''; pending.value = null
  dialogOpen.value = true
}
function cancelDialog() { if (!working.value) resetDialog() }

function confirmChange() {
  const request = pending.value
  if (!request || !props.ready) return false
  const current = props.profiles.find(space => space.id === request.profile_id)
  const confirmed = request.operation === 'delete' ? !current :
    !!current && (request.operation === 'rename' ? current.name === request.name : current.archived === (request.operation === 'remove'))
  if (!confirmed) return false
  error.value = ''
  message.value = request.operation === 'rename' ? t('spaces.renamed', { name: request.name }) :
    request.operation === 'remove' ? t('spaces.removed', { name: request.previousName }) :
    request.operation === 'delete' ? [t('spaces.deleted', { name: request.previousName }),
      request.keptNetwork ? t('spaces.delete_kept_network', { network: request.keptNetwork }) : ''].filter(Boolean).join(' ') :
    t('spaces.restored', { name: request.previousName })
  toast(message.value, 'success')
  pending.value = null
  if (request.operation === 'rename') closeRename()
  return true
}
watch(() => [props.profiles, props.ready], () => { if (!working.value) confirmChange() })

async function submit(request, previousName) {
  pending.value = { ...request, previousName }
  working.value = true; emit('busy', true); error.value = ''; message.value = ''
  try {
    const response = await fetch('./api/multiseat/profiles/manage', { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(request) })
    const result = await response.json()
    if (result?.profile_id !== request.profile_id || (response.status === 202 ? result.status !== false : !response.ok || result.status !== true))
      throw new Error(hostSentence(result) || t('spaces.change_unverified'))
    if (result.kept_network && pending.value) pending.value.keptNetwork = result.kept_network
    message.value = t('spaces.confirming')
  } catch (cause) { error.value = cause.message || t('spaces.change_failed') }
  try {
    const verified = await props.refresh()
    await nextTick()
    if (!(verified && confirmChange()) && !error.value) message.value = t('spaces.change_unconfirmed')
  } catch { error.value = t('spaces.refresh_failed') }
  finally { working.value = false; emit('busy', false) }
}
async function submitRename() {
  if (props.locked || working.value || !renaming.value || !validName.value) return
  submitted.value = true
  await submit({ operation: 'rename', profile_id: renaming.value.id, name: name.value.trim() }, renaming.value.name)
}
async function confirmDialog() {
  if (props.locked || working.value || !dialogSpace.value) return
  const space = dialogSpace.value
  const forGood = removingForGood.value
  if (forGood && !removalReady.value) return
  const request = forGood ?
    { operation: 'delete', profile_id: space.id, confirm_name: typedName.value, request_id: requestId.value } :
    { operation: dialogOperation.value, profile_id: space.id }
  await submit(request, space.name)
  dialogOpen.value = false
  resetDialog()
}
</script>
