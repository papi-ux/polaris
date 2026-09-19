<template>
  <section v-if="state.enabled || loadError" class="section-card" aria-labelledby="profile-assignment-title"
           :aria-busy="loading || creating || managing || !!saving">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div class="min-w-0">
        <h2 id="profile-assignment-title" class="section-title">{{ $t('spaces.your_spaces') }}</h2>
        <p class="mt-1 max-w-2xl text-sm text-storm">{{ $t('spaces.a_space_keeps') }}</p>
      </div>
      <div v-if="state.enabled" class="flex flex-wrap items-center gap-2">
        <span class="meta-pill">{{ countLabel }}</span>
        <span v-if="state.capacity" class="control-chip" data-spaces-capacity>
          {{ $t('spaces.capacity', { active: state.capacity.concurrent_active, limit: state.capacity.concurrent_limit }) }}
        </span>
      </div>
    </div>
    <p v-if="message" class="mt-4 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="actionError" class="mt-4 text-sm text-warning-bright" role="alert">{{ actionError }}</p>
    <p v-if="loadError" class="mt-4 text-sm text-warning-bright" role="alert">{{ loadError }}</p>
    <p v-if="state.failed" class="mt-4 text-sm text-warning-bright" role="alert">{{ $t('spaces.restore_failed') }}</p>
    <p v-else-if="state.changing" class="mt-4 text-sm text-storm" role="status">{{ $t('spaces.applying') }}</p>
    <p v-else-if="state.enabled && !state.available" class="mt-4 text-sm text-storm" role="status">{{ $t('spaces.unavailable') }}</p>
    <p v-if="state.enabled && clientsReady && !devices.length" class="mt-4 text-sm text-storm">{{ $t('spaces.pair_first') }}</p>
    <p v-if="streamLock" :id="lockReasonId" class="mt-4 text-sm text-warning-bright" role="status" data-stream-lock>{{ streamLock }}</p>
    <SpacesList v-if="state.enabled" :profiles="state.profiles" :clients="clients" :manageable="state.management_available"
                :access-available="state.access_available" :creation-available="state.creation_available"
                :removal-available="state.removal_available"
                :runtime-move-available="state.runtime_move_available" :runtime-move-job="state.runtime_move_job"
                :activity="loadError ? null : state.activity" :refreshing="loading"
                :locked="locked" :lock-reason-id="streamLock ? lockReasonId : ''" :ready="ready" :refresh="loadProfiles"
                @busy="managing = $event" @open-default="openDefault" />
    <MultiseatProfileCreate v-if="state.enabled && state.creation_available" :profiles="state.profiles"
                           :locked="locked" :ready="ready" :refreshing="loading" :refresh="loadProfiles" @busy="creating = $event" />
    <DesktopAccess v-if="Array.isArray(state.desktop_clients)" :clients="clients" :allowed="state.desktop_clients"
                   :locked="locked" :refresh="loadProfiles" @busy="managing = $event" />
    <details v-if="state.enabled && devices.length" id="spaces-default" ref="defaultSection"
             class="settings-disclosure mt-5 border-t border-storm/20 pt-3" :open="defaultOpen" @toggle="defaultOpen = $event.target.open">
      <summary class="settings-disclosure-summary focus-ring cursor-pointer rounded py-2 font-semibold text-silver">
        <span>{{ $t('spaces.default_space') }}</span>
        <span class="flex items-center gap-2">
          <span class="control-chip">{{ devices.length }}</span>
          <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7" />
          </svg>
        </span>
      </summary>
      <p class="mt-2 text-sm text-storm">{{ $t('spaces.default_space_copy') }}</p>
      <div class="mt-4 grid gap-3">
        <div v-for="client in devices" :key="client.uuid" class="min-w-0 rounded-xl border border-storm/20 bg-deep/40 p-4">
          <div class="flex flex-wrap items-start justify-between gap-2">
            <label v-if="eligible(client)" :for="'gaming-profile-' + client.uuid" class="min-w-0 break-words text-sm font-semibold text-silver">
              {{ deviceName(client) }}
            </label>
            <p v-else class="min-w-0 break-words text-sm font-semibold text-silver">{{ deviceName(client) }}</p>
            <span v-if="eligible(client) && dirty(client.uuid)" class="text-xs text-warning-bright">{{ $t('spaces.unsaved') }}</span>
          </div>
          <p :id="'gaming-profile-current-' + client.uuid" class="mt-1 break-words text-xs text-storm">
            {{ $t('spaces.default_current', { space: profileName(current(client.uuid)) }) }}
          </p>
          <div v-if="eligible(client)" class="mt-3 flex flex-col gap-2 sm:flex-row sm:items-center">
            <select :id="'gaming-profile-' + client.uuid" v-model="choices[client.uuid]"
                    class="settings-input min-w-0 text-sm sm:flex-1"
                    :aria-describedby="describedBy(client.uuid)"
                    :disabled="locked" @change="clearFeedback">
              <option v-if="offersDesktop(client.uuid)" value="desktop">{{ $t('spaces.desktop_option') }}</option>
              <option v-for="profile in spacesFor(client.uuid)" :key="profile.id" :value="profile.id">{{ profile.name }}</option>
            </select>
            <Button variant="outline" size="sm" class="shrink-0" :loading="saving === client.uuid"
                    :aria-label="$t('spaces.save_assignment_aria', { device: deviceName(client) })"
                    :aria-describedby="streamLock ? lockReasonId : undefined"
                    :disabled="locked || !dirty(client.uuid)" @click="save(client.uuid)">
              {{ saving === client.uuid ? $t('spaces.saving') : $t('spaces.save_assignment') }}
            </Button>
          </div>
          <div v-else class="mt-3">
            <Button variant="outline" size="sm" :loading="saving === client.uuid"
                    :aria-label="$t('spaces.remove_from_spaces_aria', { device: deviceName(client) })"
                    :aria-describedby="describedBy(client.uuid)"
                    :disabled="locked" data-remove-from-spaces @click="save(client.uuid, '')">
              {{ saving === client.uuid ? $t('spaces.saving') : $t('spaces.remove_from_spaces') }}
            </Button>
          </div>
          <p :id="'gaming-profile-help-' + client.uuid" class="mt-2 break-words text-xs text-storm">
            {{ selectionHelp(client) }}
          </p>
          <p v-if="eligible(client) && spacesFor(client.uuid).length && !offersDesktop(client.uuid)"
             class="mt-1 break-words text-xs text-storm" data-desktop-needs-access>
            {{ $t('spaces.help_desktop_needs_access') }}
          </p>
        </div>
      </div>
    </details>
    <div class="mt-4 flex flex-wrap items-center justify-between gap-3">
      <p v-if="state.enabled && devices.length" class="text-xs text-storm">{{ $t('spaces.one_device') }}</p>
      <Button variant="ghost" size="sm" :loading="loading" :disabled="!!saving || creating || managing" data-spaces-refresh @click="refresh">
        {{ loading ? $t('spaces.refreshing') : $t('spaces.refresh') }}
      </Button>
    </div>
  </section>
</template>

<script setup>
import { computed, inject, nextTick, onMounted, reactive, ref, watch } from 'vue'
import Button from './Button.vue'
import SpacesList from './SpacesList.vue'
import DesktopAccess from './DesktopAccess.vue'
import MultiseatProfileCreate from './MultiseatProfileCreate.vue'
import { useToast } from '../composables/useToast.js'
import { useSpacesSnapshot } from '../composables/useSpacesSnapshot.js'
import { permissionMapping } from '../composables/useClients.js'
import { deviceNameLabels } from '../device-names.js'

const emit = defineEmits(['snapshot'])
const props = defineProps({
  clients: { type: Array, default: () => [] },
  clientsReady: { type: Boolean, default: true },
})
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const { toast } = useToast()
const choices = reactive({})
const saving = ref(''), creating = ref(false), managing = ref(false)
const actionError = ref(''), message = ref('')
const defaultOpen = ref(false), defaultSection = ref(null)
const lockReasonId = 'spaces-stream-lock'

const { state, loading, loadError, load, start } = useSpacesSnapshot({
  busy: () => !!saving.value || creating.value || managing.value,
  onSnapshot: afterLoad,
  messages: { load: t('spaces.load_failed'), verify: t('spaces.verify_failed') },
})

const activeSpaces = computed(() => state.profiles.filter(space => !space.archived))
const countLabel = computed(() => t(activeSpaces.value.length === 1 ? 'spaces.count_one' : 'spaces.count_many', { count: activeSpaces.value.length }))
const eligible = client => !client.temporary_authorization && (Number(client.perm) & permissionMapping.launch) !== 0
const hasDesktopAccess = id => (state.desktop_clients || []).includes(id)
// The Spaces a device may open: its Default Space and every Space that allows it under Device Access.
const spacesFor = id => activeSpaces.value.filter(space => space.clients.includes(id) || (space.access_clients || []).includes(id))
// A default is only ever a place the device may play. Desktop is one with Desktop Access, or when
// the device has no Space at all.
const offersDesktop = id => hasDesktopAccess(id) || !spacesFor(id).length
const choicesFor = id => [...(offersDesktop(id) ? ['desktop'] : []), ...spacesFor(id).map(space => space.id)]
// Where the device opens first, in the host's order: a Desktop default, its Default Space, then
// the first Space it may open, then Desktop.
const current = id => {
  if ((state.desktop_default_clients || []).includes(id) && hasDesktopAccess(id)) return 'desktop'
  return activeSpaces.value.find(profile => profile.clients.includes(id))?.id || spacesFor(id)[0]?.id || 'desktop'
}
const profileName = id => (id !== 'desktop' && state.profiles.find(profile => profile.id === id)?.name) || t('spaces.desktop')
const nameLabels = computed(() => deviceNameLabels(props.clients, { t, fallback: t('spaces.paired_device') }))
const deviceName = client => nameLabels.value.get(client?.uuid) || client?.friendly_name || client?.name || t('spaces.paired_device')
const devices = computed(() => props.clients.filter(client => eligible(client) || spacesFor(client.uuid).length))
const dirty = id => choices[id] !== current(id)
const ready = computed(() => state.available && !state.changing && !state.failed && !loadError.value)
// A live Space stream holds the catalog: every change waits for it, and the
// controls say so instead of sending the request to a 409.
const streamActivity = computed(() => Array.isArray(state.activity) ? state.activity : [])
const streamLock = computed(() => {
  const item = streamActivity.value[0]
  if (!item) return ''
  const device = props.clients.find(client => client.uuid === item.client_id)
  const name = device ? deviceName(device) : t('spaces.paired_device')
  return t(item.state === 'stopping' ? 'spaces.stream_lock_stopping' : 'spaces.stream_lock_running', { device: name })
})
const locked = computed(() => !!saving.value || creating.value || managing.value || !!loadError.value ||
  state.changing || state.failed || !state.available || streamActivity.value.length > 0)
const describedBy = uuid => ['gaming-profile-current-' + uuid, 'gaming-profile-help-' + uuid, streamLock.value ? lockReasonId : ''].filter(Boolean).join(' ')

function clearFeedback() { message.value = ''; actionError.value = '' }

function selectionHelp(client) {
  if (!eligible(client)) return t('spaces.help_lost_access')
  if (!spacesFor(client.uuid).length) return t('spaces.help_no_space')
  const selected = activeSpaces.value.find(profile => profile.id === choices[client.uuid])
  if (!selected) return t('spaces.help_desktop')
  const others = selected.clients.filter(id => id !== client.uuid)
  if (!others.length) return t('spaces.help_keeps')
  const names = others.map(id => {
    const device = props.clients.find(item => item.uuid === id)
    return device ? deviceName(device) : t('spaces.another_device')
  })
  return t('spaces.help_shared', { devices: names.join(', ') })
}

function reconcileChoices(resetClient = '') {
  for (const id of Object.keys(choices)) {
    if (!props.clients.some(client => client.uuid === id)) delete choices[id]
  }
  for (const client of props.clients) {
    const choice = choices[client.uuid]
    if (client.uuid === resetClient || choice === undefined || !choicesFor(client.uuid).includes(choice)) {
      choices[client.uuid] = current(client.uuid)
    }
  }
}
watch(() => props.clients, () => reconcileChoices(), { deep: true })

let resetAfterLoad = ''
let edited = new Set()
function afterLoad(next) {
  emit('snapshot', { ...next })
  for (const client of props.clients) {
    if (!edited.has(client.uuid)) choices[client.uuid] = current(client.uuid)
  }
  reconcileChoices(resetAfterLoad)
  resetAfterLoad = ''
  edited = new Set()
  // A Space nobody can open yet is the next step; open the section for it once.
  if (activeSpaces.value.some(space => !space.clients.length && !(space.access_clients || []).length)) defaultOpen.value = true
}

async function loadProfiles(resetClient = '') {
  resetAfterLoad = resetClient
  edited = new Set(props.clients.filter(client => dirty(client.uuid) && choices[client.uuid] !== undefined).map(client => client.uuid))
  const ok = await load()
  if (!ok) { resetAfterLoad = ''; edited = new Set() }
  return ok
}

async function refresh() {
  if (saving.value || creating.value || managing.value || loading.value) return
  clearFeedback()
  await loadProfiles()
}

async function openDefault() {
  defaultOpen.value = true
  await nextTick()
  const section = defaultSection.value
  if (!section) return
  section.scrollIntoView?.({ block: 'start', behavior: 'smooth' })
  section.querySelector('select')?.focus()
}

// requested is a Space id or 'desktop' for a default, or '' to remove the device from every Space.
async function save(client, requested = choices[client]) {
  const removal = requested === ''
  if (locked.value || (!removal && !dirty(client))) return
  saving.value = client
  clearFeedback()
  try {
    const response = await fetch('./api/multiseat/assign', {
      credentials: 'include', method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ client_id: client, profile_id: requested }),
    })
    const result = await response.json()
    if (response.status !== 202 && (!response.ok || result.status !== true)) {
      throw new Error(result.message || result.error || t('spaces.assignment_failed'))
    }
    const verified = await loadProfiles(client)
    if (!verified) return
    const confirmed = removal ? !spacesFor(client).length : current(client) === requested
    if (state.enabled && state.available && !state.changing && !state.failed && confirmed) {
      const name = deviceName(props.clients.find(item => item.uuid === client) || {})
      message.value = removal ? t('spaces.removed_from_spaces', { device: name })
        : requested === 'desktop' ? t('spaces.assignment_saved_desktop', { device: name })
        : t('spaces.assignment_saved', { device: name, space: profileName(requested) })
      toast(message.value, 'success')
    } else if (response.status === 202 || state.changing) {
      message.value = t('spaces.assignment_pending')
    } else {
      actionError.value = t('spaces.assignment_unconfirmed')
    }
  } catch (cause) {
    actionError.value = cause.message || t('spaces.assignment_failed')
    await loadProfiles(client)
  } finally { saving.value = '' }
}

onMounted(() => { start() })
</script>
