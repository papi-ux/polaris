<template>
  <!-- One table for what used to be five lists: Device Access on every Space card, Desktop Access,
       and Default Space. A row is a device, a tick column is a place it may open, and the last
       column is where it opens first. Narrow containers show the same rows as one card per device. -->
  <!-- With no device left to list, the section stays for as long as it has an outcome to report. -->
  <section v-if="rows.length || message || error" class="mt-5 border-t border-storm/20 pt-4" aria-labelledby="spaces-devices-title" data-spaces-devices>
    <div class="flex items-start justify-between gap-3">
      <div class="min-w-0">
        <h3 id="spaces-devices-title" class="font-semibold text-silver">{{ $t('spaces.device_access') }}</h3>
        <p v-if="rows.length" class="mt-1 max-w-3xl text-sm text-storm">{{ $t('spaces.devices_copy') }}</p>
      </div>
      <span v-if="rows.length" class="control-chip shrink-0" data-devices-count>{{ rows.length }}</span>
    </div>
    <label v-if="rows.length && byDefault !== undefined" class="mt-3 flex items-start gap-3 rounded-xl border border-storm/20 bg-deep/40 p-3 text-sm text-silver">
      <input type="checkbox" role="switch" class="mt-0.5 h-4 w-4 shrink-0 rounded border-storm bg-void text-ice accent-ice"
             :checked="byDefault" :disabled="blocked" :aria-describedby="lockReasonId || undefined"
             data-desktop-by-default @change="saveByDefault">
      <span class="min-w-0">
        {{ $t('spaces.desktop_by_default') }}
        <span class="mt-0.5 block text-xs text-storm">{{ $t('spaces.desktop_by_default_copy') }}</span>
      </span>
    </label>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <div v-if="rows.length" class="devices-wrap mt-4">
      <table class="devices-table" role="table" aria-labelledby="spaces-devices-title">
        <thead role="rowgroup">
          <tr role="row">
            <th role="columnheader" scope="col">{{ $t('spaces.devices_col_device') }}</th>
            <th v-for="column in columns" :key="column.id" role="columnheader" scope="col" class="devices-tick" :data-devices-column="column.id">
              {{ column.name }}
            </th>
            <th role="columnheader" scope="col">{{ $t('spaces.default_space') }}</th>
          </tr>
        </thead>
        <tbody role="rowgroup">
          <!-- One host change for every device, where ticking them one by one restarts Spaces each time. -->
          <tr v-if="bulk" role="row" class="devices-bulk" data-devices-bulk>
            <th role="rowheader" scope="row" class="devices-name text-storm">{{ $t('spaces.devices_every') }}</th>
            <td v-for="column in columns" :key="column.id" role="cell" class="devices-tick" :data-access-bulk="column.id">
              <span class="devices-cell-label">{{ column.name }}</span>
              <span class="inline-flex items-center gap-1">
                <Button variant="ghost" size="sm" class="px-2" :disabled="blocked || allowedCount(column) === eligible.length"
                        :aria-label="column.desktop ? $t('spaces.desktop_select_all_aria') : $t('spaces.access_select_all_aria', { space: column.name })"
                        :aria-describedby="lockReasonId || undefined" data-access-select-all @click="setAll(column, true)">
                  {{ $t('spaces.access_all') }}
                </Button>
                <Button variant="ghost" size="sm" class="px-2" :disabled="blocked || !listedCount(column)"
                        :aria-label="column.desktop ? $t('spaces.desktop_clear_all_aria') : $t('spaces.access_clear_all_aria', { space: column.name })"
                        :aria-describedby="lockReasonId || undefined" data-access-clear-all @click="clearing = column">
                  {{ $t('spaces.access_none') }}
                </Button>
              </span>
            </td>
            <td role="cell" class="devices-default devices-blank"></td>
          </tr>
          <tr v-for="client in rows" :key="client.uuid" role="row" :data-device-row="client.uuid">
            <th role="rowheader" scope="row" class="devices-name">
              <label v-if="choosable(client)" :for="'gaming-profile-' + client.uuid" class="break-words" data-device-name>{{ deviceName(client) }}</label>
              <span v-else class="break-words" data-device-name>{{ deviceName(client) }}</span>
              <span v-if="!canLaunch(client)" :id="'gaming-profile-help-' + client.uuid" class="mt-0.5 block break-words text-xs font-normal text-warning-bright">
                {{ $t('spaces.help_lost_access') }}
              </span>
            </th>
            <td v-for="column in columns" :key="column.id" role="cell" class="devices-tick">
              <label class="devices-tick-label" :class="blocked || !canLaunch(client) ? 'cursor-not-allowed' : 'cursor-pointer'">
                <span class="devices-cell-label">{{ column.name }}</span>
                <input type="checkbox" class="h-4 w-4 shrink-0 rounded border-storm bg-void text-ice accent-ice"
                       :checked="ticked(column, client.uuid)" :disabled="blocked || !canLaunch(client)"
                       :aria-label="column.desktop ? $t('spaces.desktop_allow_aria', { device: deviceName(client) })
                         : $t('spaces.allow_aria', { device: deviceName(client), space: column.name })"
                       :aria-describedby="lockReasonId || undefined" @change="tick(column, client.uuid, $event)">
              </label>
            </td>
            <td role="cell" class="devices-default">
              <span class="devices-cell-label">{{ $t('spaces.default_space') }}</span>
              <Button v-if="!canLaunch(client)" variant="outline" size="sm" :loading="working === 'default:' + client.uuid"
                      :aria-label="$t('spaces.remove_from_spaces_aria', { device: deviceName(client) })"
                      :aria-describedby="describedBy(client.uuid, false)" :disabled="blocked"
                      data-remove-from-spaces @click="saveDefault(client.uuid, '')">
                {{ working === 'default:' + client.uuid ? $t('spaces.saving') : $t('spaces.remove_from_spaces') }}
              </Button>
              <div v-else-if="choosable(client)" class="min-w-0">
                <div class="flex min-w-0 items-center gap-2">
                  <select :id="'gaming-profile-' + client.uuid" class="settings-input min-w-0 flex-1 py-1.5 text-sm"
                          :aria-describedby="describedBy(client.uuid)" :disabled="blocked"
                          @change="draft(client.uuid, $event.target.value)">
                    <option v-for="place in placesFor(state, client.uuid)" :key="place" :value="place" :selected="choice(client.uuid) === place">
                      {{ placeName(place) }}
                    </option>
                  </select>
                  <Button v-if="dirty(client.uuid)" variant="outline" size="sm" class="shrink-0" :loading="working === 'default:' + client.uuid"
                          :aria-label="$t('spaces.save_assignment_aria', { device: deviceName(client) })"
                          :aria-describedby="lockReasonId || undefined" :disabled="blocked" @click="saveDefault(client.uuid)">
                    {{ working === 'default:' + client.uuid ? $t('spaces.saving') : $t('_common.save') }}
                  </Button>
                </div>
                <!-- Always in the page, because the dropdown is described by them; shown while the
                     row holds a choice that has not been saved. -->
                <p :id="'gaming-profile-current-' + client.uuid" :class="dirty(client.uuid) ? 'mt-1 break-words text-xs text-warning-bright' : 'sr-only'">
                  <template v-if="dirty(client.uuid)">{{ $t('spaces.unsaved') }}. </template>{{ $t('spaces.default_current', { space: placeName(opensFirst(state, client.uuid)) }) }}
                </p>
                <p :id="'gaming-profile-help-' + client.uuid" :class="dirty(client.uuid) ? 'mt-1 break-words text-xs text-storm' : 'sr-only'">
                  {{ selectionHelp(client) }}
                </p>
              </div>
              <!-- One place to play is not a choice, so it is said rather than offered. -->
              <span v-else class="break-words text-storm" data-default-only>{{ placeName(opensFirst(state, client.uuid)) }}</span>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
    <ConfirmActionDialog :model-value="!!clearing" :title="clearing?.desktop ? $t('spaces.desktop_clear_title') : $t('spaces.access_clear_title', { space: clearing?.name })"
                         :message="clearing?.desktop ? $t('spaces.desktop_clear_message') : $t('spaces.access_clear_message')"
                         :impact-items="clearing?.desktop ? [$t('spaces.desktop_clear_impact_default'), $t('spaces.desktop_clear_impact_kept')]
                           : [$t('spaces.access_clear_impact_default'), $t('spaces.access_clear_impact_kept')]"
                         :confirm-label="$t('spaces.access_remove_all')" :cancel-label="$t('spaces.cancel')"
                         :pending-label="$t('spaces.saving')" :pending="!!working"
                         :eyebrow="$t('spaces.kicker')" :impact-label="$t('spaces.dialog_impact')"
                         @update:model-value="open => { if (!open && !working) clearing = null }" @confirm="setAll(clearing, false)" />
  </section>
</template>

<script setup>
import { computed, inject, nextTick, reactive, ref, watch } from 'vue'
import Button from './Button.vue'
import ConfirmActionDialog from './ConfirmActionDialog.vue'
import { useToast } from '../composables/useToast.js'
import { deviceNameLabels } from '../device-names.js'
import { setAccessForAll } from '../spaces-bulk-access.js'
import { activeSpaces, canLaunch, hasDesktop, inSpace, listedDevices, opensFirst, placesFor } from '../spaces-devices.js'

// state is the Spaces snapshot. byDefault is the owner's "a device with a Space also gets Desktop"
// setting; a host from before 1.4.12 sends none, and the switch is then left out rather than shown off.
const props = defineProps({ state: { type: Object, required: true }, clients: { type: Array, default: () => [] },
  byDefault: { type: Boolean, default: undefined }, accessAvailable: Boolean,
  locked: Boolean, lockReasonId: { type: String, default: '' }, refresh: { type: Function, required: true } })
const emit = defineEmits(['busy'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const { toast } = useToast()
// What is being saved: 'default:<device>', 'access', 'all' or 'setting'. Everything waits for it.
const working = ref(''), message = ref(''), error = ref(''), clearing = ref(null)
// A Default Space picked but not saved yet, by device. Anything not in here follows the host.
const drafts = reactive({})

const blocked = computed(() => props.locked || !!working.value)
const settled = computed(() => props.state.available && !props.state.changing && !props.state.failed)
const rows = computed(() => listedDevices(props.state, props.clients))
const eligible = computed(() => props.clients.filter(canLaunch))
const columns = computed(() => [
  ...(Array.isArray(props.state.desktop_clients) ? [{ id: 'desktop', name: t('spaces.desktop'), desktop: true }] : []),
  ...(props.accessAvailable ? activeSpaces(props.state).map(space => ({ id: space.id, name: space.name, space })) : []),
])
const bulk = computed(() => eligible.value.length > 1 && columns.value.length > 0)
const nameLabels = computed(() => deviceNameLabels(props.clients, { t, fallback: t('spaces.paired_device') }))
const deviceName = client => nameLabels.value.get(client?.uuid) || client?.friendly_name || client?.name || t('spaces.paired_device')
const placeName = id => (id !== 'desktop' && props.state.profiles.find(space => space.id === id)?.name) || t('spaces.desktop')

const ticked = (column, id) => column.desktop ? hasDesktop(props.state, id) : inSpace(column.space, id)
const allowedCount = column => eligible.value.filter(client => ticked(column, client.uuid)).length
// Everything the host lists in the column, a device that was unpaired since included: None empties
// that too, so it is offered while anything at all is listed.
const listedCount = column => column.desktop ? props.state.desktop_clients.length
  : new Set([...(column.space.clients || []), ...(column.space.access_clients || [])]).size

const choosable = client => canLaunch(client) && placesFor(props.state, client.uuid).length > 1
const choice = id => placesFor(props.state, id).includes(drafts[id]) ? drafts[id] : opensFirst(props.state, id)
const dirty = id => choice(id) !== opensFirst(props.state, id)
// A row that cannot choose has no "Default:" line, so its button is not described by one.
const describedBy = (id, choosing = true) => [choosing ? 'gaming-profile-current-' + id : '', 'gaming-profile-help-' + id, props.lockReasonId].filter(Boolean).join(' ')
function draft(id, place) {
  message.value = ''; error.value = ''
  if (place === opensFirst(props.state, id)) delete drafts[id]
  else drafts[id] = place
}
// A draft goes when the host caught up with it, when its place stopped being one the device may
// open, or when the device was unpaired.
watch(() => [props.state.profiles, props.state.desktop_clients, props.state.desktop_default_clients, props.clients], () => {
  for (const id of Object.keys(drafts)) {
    if (!props.clients.some(client => client.uuid === id) || !dirty(id)) delete drafts[id]
  }
})

function selectionHelp(client) {
  const selected = activeSpaces(props.state).find(space => space.id === choice(client.uuid))
  if (!selected) return t('spaces.help_desktop')
  const others = selected.clients.filter(id => id !== client.uuid)
  if (!others.length) return t('spaces.help_keeps')
  const names = others.map(id => {
    const device = props.clients.find(item => item.uuid === id)
    return device ? deviceName(device) : t('spaces.another_device')
  })
  return t('spaces.help_shared', { devices: names.join(', ') })
}

// Every change is sent, then read back from the host, and only what the host shows is claimed.
async function post(url, body, fallback) {
  const response = await fetch(url, { method: 'POST', credentials: 'include',
    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })
  const result = await response.json().catch(() => null)
  if (response.status !== 202 && (!response.ok || result?.status !== true)) throw new Error(result?.message || result?.error || fallback)
  return response.status === 202
}
async function change(key, send, outcome, fallback) {
  if (blocked.value) return
  working.value = key; emit('busy', true); message.value = ''; error.value = ''
  try {
    const pending = await send()
    const verified = await props.refresh()
    await nextTick()
    const said = outcome({ pending, verified })
    if (said?.error) error.value = said.error
    else if (said?.message) { message.value = said.message; if (said.confirmed) toast(said.message, 'success') }
  } catch (cause) {
    error.value = cause.unsupported ? t('spaces.access_all_unsupported') : cause.message || fallback
    toast(error.value, 'error', 6000)
    try { await props.refresh() } catch { /* Keep the failed request visible. */ }
  } finally { working.value = ''; emit('busy', false); clearing.value = null }
}

function tick(column, client, event) {
  const requested = event.target.checked
  // The box shows what the host has until the host says otherwise.
  event.target.checked = ticked(column, client)
  const fallback = t(column.desktop ? 'spaces.desktop_failed' : 'spaces.access_failed')
  return change('access', () => post('./api/multiseat/access', { profile_id: column.id, client_id: client, allowed: requested }, fallback),
    ({ verified }) => {
      const now = columns.value.find(item => item.id === column.id)
      const confirmed = verified && settled.value && !!now && ticked(now, client) === requested
      return confirmed ? { message: t(column.desktop ? 'spaces.desktop_saved' : 'spaces.access_saved'), confirmed }
        : { message: t('spaces.access_unconfirmed') }
    }, fallback)
}

function setAll(column, requested) {
  if (!column) return
  const fallback = t(column.desktop ? 'spaces.desktop_failed' : 'spaces.access_failed')
  return change('all', async () => (await setAccessForAll(column.id, requested)).pending,
    ({ pending, verified }) => {
      const now = columns.value.find(item => item.id === column.id)
      const done = !!now && (requested ? allowedCount(now) === eligible.value.length : listedCount(now) === 0)
      if (pending || !verified || !settled.value || !done) return { message: t('spaces.access_unconfirmed') }
      const said = column.desktop
        ? (requested ? t('spaces.desktop_all_saved', { count: eligible.value.length }) : t('spaces.desktop_cleared'))
        : (requested ? t('spaces.access_all_saved', { count: eligible.value.length, space: column.name }) : t('spaces.access_cleared', { space: column.name }))
      return { message: said, confirmed: true }
    }, fallback)
}

// The setting changes what later access changes do and nothing else, so it restarts nothing.
function saveByDefault(event) {
  const requested = event.target.checked
  event.target.checked = props.byDefault === true
  const fallback = t('spaces.desktop_by_default_failed')
  return change('setting', () => post('./api/multiseat/settings', { desktop_by_default: requested }, fallback),
    ({ verified }) => verified && props.byDefault === requested
      ? { message: t(requested ? 'spaces.desktop_by_default_on' : 'spaces.desktop_by_default_off'), confirmed: true }
      : { message: t('spaces.access_unconfirmed') }, fallback)
}

// requested is a Space id or 'desktop' for a default, or '' to remove the device from every Space.
function saveDefault(client, requested = choice(client)) {
  const removal = requested === ''
  if (!removal && !dirty(client)) return
  const fallback = t('spaces.assignment_failed')
  return change('default:' + client, async () => {
    try { return await post('./api/multiseat/assign', { client_id: client, profile_id: requested }, fallback) }
    // A refused choice goes back to what the host has, so the row does not keep offering it.
    catch (cause) { delete drafts[client]; throw cause }
  }, ({ pending, verified }) => {
    delete drafts[client]
    // A failed read-back is the section's to report, and it locks the table until a refresh works.
    if (!verified) return null
    const confirmed = removal ? !placesFor(props.state, client).some(place => place !== 'desktop') : opensFirst(props.state, client) === requested
    if (settled.value && confirmed) {
      const name = deviceName(props.clients.find(item => item.uuid === client) || {})
      return { confirmed, message: removal ? t('spaces.removed_from_spaces', { device: name })
        : requested === 'desktop' ? t('spaces.assignment_saved_desktop', { device: name })
        : t('spaces.assignment_saved', { device: name, space: placeName(requested) }) }
    }
    return pending || props.state.changing ? { message: t('spaces.assignment_pending') } : { error: t('spaces.assignment_unconfirmed') }
  }, fallback)
}
</script>
