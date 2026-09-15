<template>
  <section v-if="state.enabled || loadError" class="section-card" aria-labelledby="profile-assignment-title" :aria-busy="loading || creating || managing || !!saving">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div class="min-w-0">
        <h2 id="profile-assignment-title" class="section-title">Your Spaces</h2>
        <p class="mt-2 max-w-2xl text-sm text-storm">
          A space keeps its own Steam sign-in, games, and saves. Open your assigned space from Library in Nova.
        </p>
      </div>
      <span v-if="state.enabled" class="meta-pill">{{ activeSpaces.length }} {{ activeSpaces.length === 1 ? 'space' : 'spaces' }}</span>
    </div>
    <p v-if="message" class="mt-4 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="actionError" class="mt-4 text-sm text-warning-bright" role="alert">{{ actionError }}</p>
    <p v-if="loadError" class="mt-4 text-sm text-warning-bright" role="alert">{{ loadError }}</p>
    <p v-if="state.failed" class="mt-4 text-sm text-warning-bright" role="alert">
      Space settings could not be restored. Review the configuration and restart Polaris.
    </p>
    <p v-else-if="state.changing" class="mt-4 text-sm text-storm" role="status">
      Polaris is applying space changes. Refresh spaces to check when they are ready.
    </p>
    <p v-else-if="state.enabled && !state.available" class="mt-4 text-sm text-storm" role="status">
      Spaces are temporarily unavailable. Refresh spaces to try again.
    </p>
    <p v-if="state.enabled && clientsReady && !devices.length" class="mt-4 text-sm text-storm">
      Pair a device with permission to launch apps to assign a space. Temporary guests cannot use these spaces.
    </p>
    <SpacesList v-if="state.enabled" :profiles="state.profiles" :clients="clients" :manageable="state.management_available" :access-available="state.access_available"
                :activity="loadError ? null : state.activity" :refreshing="loading"
                :locked="locked" :ready="state.available && !state.changing && !state.failed && !loadError" :refresh="loadProfiles" @busy="managing = $event" />
    <MultiseatProfileCreate v-if="state.enabled && state.creation_available" :profiles="state.profiles"
                           :locked="locked" :ready="state.available && !state.changing && !state.failed && !loadError"
                           :refreshing="loading" :refresh="loadProfiles" @busy="creating = $event" />
    <DesktopAccess v-if="Array.isArray(state.desktop_clients)" :clients="clients" :allowed="state.desktop_clients"
                   :locked="locked" :refresh="refresh" @busy="managing = $event" />
    <details v-if="state.enabled && devices.length" class="mt-5 border-t border-storm/20 pt-3" open>
      <summary class="focus-ring cursor-pointer rounded py-2 font-semibold text-silver">Default Space</summary>
      <p class="mt-2 text-sm text-storm">Devices are the handhelds, TVs, and computers paired with Polaris. Choose the Space each device starts with. Allow additional Spaces using Device Access on a Space card.
        Rename a device in Devices if its name is hard to recognize.</p>
      <div class="mt-4 grid gap-3">
      <div v-for="client in devices" :key="client.uuid" class="min-w-0 rounded-xl border border-storm/20 bg-deep/40 p-4">
        <div class="flex flex-wrap items-start justify-between gap-2">
          <label :for="'gaming-profile-' + client.uuid" class="min-w-0 break-words text-sm font-semibold text-silver">
            {{ deviceName(client) }}
          </label>
          <span v-if="dirty(client.uuid)" class="text-xs text-warning-bright">Unsaved change</span>
        </div>
        <p :id="'gaming-profile-current-' + client.uuid" class="mt-1 break-words text-xs text-storm">
          Default: {{ profileName(assigned(client.uuid)) }}
        </p>
        <div class="mt-3 flex flex-col gap-2 sm:flex-row sm:items-center">
          <select :id="'gaming-profile-' + client.uuid" v-model="choices[client.uuid]"
                  class="focus-ring min-w-0 w-full rounded-lg border border-storm/30 bg-deep px-3 py-2.5 text-sm text-silver sm:flex-1"
                  :aria-describedby="'gaming-profile-current-' + client.uuid + ' gaming-profile-help-' + client.uuid"
                  :disabled="locked" @change="clearFeedback">
            <option value="">This PC’s desktop and apps</option>
            <option v-for="profile in activeSpaces" :key="profile.id" :value="profile.id"
                    :disabled="!eligible(client)">{{ profile.name }}</option>
          </select>
          <button type="button" class="focus-ring shrink-0 rounded-lg border border-ice/30 px-3 py-2.5 text-sm text-ice disabled:opacity-40"
                  :aria-label="'Save assignment for ' + deviceName(client)"
                  :disabled="locked || !dirty(client.uuid)" @click="save(client.uuid)">
            {{ saving === client.uuid ? 'Saving…' : 'Save assignment' }}
          </button>
        </div>
        <p :id="'gaming-profile-help-' + client.uuid" class="mt-2 break-words text-xs text-storm">
          {{ selectionHelp(client) }}
        </p>
      </div>
      </div>
    </details>
    <div class="mt-4 flex flex-wrap items-center justify-between gap-3">
      <p v-if="state.enabled && devices.length" class="text-xs text-storm">Stop space streams before changing assignments.</p>
      <button type="button" class="focus-ring rounded-lg px-1 py-2 text-sm text-ice disabled:opacity-40"
              :disabled="!!saving || creating || managing || loading" @click="refresh">
        {{ loading ? 'Refreshing…' : 'Refresh spaces' }}
      </button>
    </div>
  </section>
</template>

<script setup>
import { computed, onMounted, onUnmounted, reactive, ref, watch } from 'vue'
import SpacesList from './SpacesList.vue'
import DesktopAccess from './DesktopAccess.vue'
import MultiseatProfileCreate from './MultiseatProfileCreate.vue'
import { validSnapshot } from '../spaces-access.js'

const emit = defineEmits(['snapshot'])
const props = defineProps({
  clients: { type: Array, default: () => [] },
  clientsReady: { type: Boolean, default: true },
})
const state = reactive({ enabled: false, available: false, changing: false, failed: false, profiles: [], activity: null, creation_available: false, management_available: false, access_available: false })
const choices = reactive({})
const saving = ref(''), loading = ref(false)
let request, disposed = false
const creating = ref(false), managing = ref(false)
const activeSpaces = computed(() => state.profiles.filter(space => !space.archived))
const loadError = ref(''), actionError = ref(''), message = ref('')
const locked = computed(() => !!saving.value || creating.value || managing.value || loading.value || !!loadError.value || state.changing || state.failed || !state.available)
const eligible = client => !client.temporary_authorization && (Number(client.perm) & 0x04000000) !== 0
const assigned = id => state.profiles.find(profile => profile.clients.includes(id))?.id ||
  state.profiles.find(profile => !profile.archived && (profile.access_clients || []).includes(id))?.id || ''
const profileName = id => state.profiles.find(profile => profile.id === id)?.name || 'This PC’s desktop and apps'
const deviceName = client => client.friendly_name || client.name || 'Paired device'
const devices = computed(() => props.clients.filter(client => eligible(client) || assigned(client.uuid)))
const dirty = id => choices[id] !== assigned(id)

function clearFeedback() { message.value = ''; actionError.value = '' }

function selectionHelp(client) {
  if (!eligible(client)) return 'This device no longer has space access. Choose This PC’s desktop and apps to remove its assignment.'
  const selected = state.profiles.find(profile => profile.id === choices[client.uuid])
  if (!selected) return 'Removes all Space access and uses the usual apps and account on this PC.'
  const others = selected.clients.filter(id => id !== client.uuid)
  if (!others.length) return 'Keeps this space’s sign-ins, saves, and settings between sessions.'
  const names = others.map(id => {
    const device = props.clients.find(item => item.uuid === id)
    return device ? deviceName(device) : 'another paired device'
  })
  return 'Also assigned to ' + names.join(', ') + '. Only one of these devices can stream this space at a time.'
}

function reconcileChoices(resetClient = '') {
  for (const id of Object.keys(choices)) {
    if (!props.clients.some(client => client.uuid === id)) delete choices[id]
  }
  for (const client of props.clients) {
    const choice = choices[client.uuid]
    if (client.uuid === resetClient || choice === undefined ||
        (choice !== '' && (!eligible(client) || !activeSpaces.value.some(profile => profile.id === choice)))) {
      choices[client.uuid] = assigned(client.uuid)
    }
  }
}
watch(() => props.clients, () => reconcileChoices(), { deep: true })


async function loadProfiles(resetClient = '') {
  if (disposed) return false
  loading.value = true
  request?.abort()
  const current = new AbortController()
  request = current
  const timeout = setTimeout(() => current.abort(), 12000)
  try {
    const response = await fetch('./api/multiseat/profiles', { credentials: 'include', cache: 'no-store', signal: current.signal })
    if (!response.ok) throw new Error('Could not load space assignments. Refresh spaces to try again.')
    const next = await response.json()
    if (disposed || request !== current) return false
    if (!validSnapshot(next)) throw new Error('Could not verify space assignments. Refresh spaces to try again.')
    const edited = new Set(props.clients.filter(client => dirty(client.uuid) && choices[client.uuid] !== undefined).map(client => client.uuid))
    Object.assign(state, { activity: null, creation_available: false, management_available: false, access_available: false }, next)
    emit('snapshot', { ...next })
    for (const client of props.clients) {
      if (!edited.has(client.uuid)) choices[client.uuid] = assigned(client.uuid)
    }
    reconcileChoices(resetClient)
    loadError.value = ''
    return true
  } catch (cause) {
    if (disposed || request !== current) return false
    emit('snapshot', null)
    loadError.value = cause.message || 'Could not load space assignments. Refresh spaces to try again.'
    return false
  } finally { clearTimeout(timeout); if (request === current) loading.value = false }
}

async function refresh() {
  if (saving.value || creating.value || managing.value || loading.value) return
  clearFeedback()
  await loadProfiles()
}

async function save(client) {
  if (locked.value || !dirty(client)) return
  const requested = choices[client]
  saving.value = client
  clearFeedback()
  try {
    const response = await fetch('./api/multiseat/assign', {
      credentials: 'include', method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ client_id: client, profile_id: requested }),
    })
    const result = await response.json()
    if (response.status !== 202 && (!response.ok || result.status !== true)) {
      throw new Error(result.message || result.error || 'Assignment was not saved.')
    }
    const verified = await loadProfiles(client)
    if (!verified) return
    if (state.enabled && state.available && !state.changing && !state.failed && assigned(client) === requested) {
      const name = deviceName(props.clients.find(item => item.uuid === client) || {})
      message.value = 'Assignment saved. ' + name + ' has default Space ' + profileName(requested) + '. Refresh the device library before starting a stream.'
    } else if (response.status === 202 || state.changing) {
      message.value = 'The assignment is still being applied. Refresh spaces to confirm it before starting a stream.'
    } else {
      actionError.value = 'The requested assignment could not be confirmed. Review the current assignment and try again.'
    }
  } catch (cause) {
    actionError.value = cause.message || 'Assignment was not saved.'
    await loadProfiles(client)
  } finally { saving.value = '' }
}
let poll
onMounted(() => {
  refresh()
  poll = setInterval(() => {
    if (document.visibilityState === 'visible' && !saving.value && !creating.value && !managing.value && !loading.value) loadProfiles()
  }, 10000)
})
onUnmounted(() => { disposed = true; clearInterval(poll); request?.abort() })
</script>
