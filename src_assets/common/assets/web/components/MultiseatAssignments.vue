<template>
  <section v-if="state.enabled || loadError" class="section-card" aria-labelledby="profile-assignment-title" :aria-busy="loading || !!saving">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div class="min-w-0">
        <h2 id="profile-assignment-title" class="section-title">Separate gaming profiles</h2>
        <p class="mt-2 max-w-2xl text-sm text-storm">
          Choose what each device opens. A gaming profile keeps its own sign-ins, saves, and settings.
          Standard streaming opens the usual apps on this PC.
        </p>
      </div>
      <span v-if="state.enabled" class="meta-pill">{{ state.profiles.length }} {{ state.profiles.length === 1 ? 'profile' : 'profiles' }}</span>
    </div>
    <details v-if="state.enabled && state.profiles.length" class="mt-4 rounded-xl border border-ice/20 bg-ice/5 p-4 text-sm text-silver">
      <summary class="focus-ring cursor-pointer rounded font-medium">Sharing profiles and Steam sign-in</summary>
      <p class="mt-3 text-storm">
        Your handheld and TV can share a profile, with one stream at a time.
        Use one profile per player to play at the same time.
      </p>
      <p class="mt-2 text-storm">
        On a Steam profile, open Big Picture and sign in when prompted. The profile keeps that sign-in for future sessions.
      </p>
    </details>
    <p v-if="message" class="mt-4 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="actionError" class="mt-4 text-sm text-warning-bright" role="alert">{{ actionError }}</p>
    <p v-if="loadError" class="mt-4 text-sm text-warning-bright" role="alert">{{ loadError }}</p>
    <p v-if="state.failed" class="mt-4 text-sm text-warning-bright" role="alert">
      Profile settings could not be restored. Review the configuration and restart Polaris.
    </p>
    <p v-else-if="state.changing" class="mt-4 text-sm text-storm" role="status">
      Polaris is applying profile changes. Refresh profiles to check when they are ready.
    </p>
    <p v-else-if="state.enabled && !state.available" class="mt-4 text-sm text-storm" role="status">
      Profiles are temporarily unavailable. Refresh profiles to try again.
    </p>
    <p v-if="state.enabled && !state.profiles.length" class="mt-4 text-sm text-storm">
      No gaming profiles are configured yet. Devices can continue using Standard streaming.
    </p>
    <p v-if="state.enabled && !devices.length" class="mt-4 text-sm text-storm">
      Pair a device with permission to launch apps to assign a gaming profile. Temporary guests cannot use these profiles.
    </p>
    <div v-if="state.enabled && devices.length" class="mt-5 grid gap-3">
      <div v-for="client in devices" :key="client.uuid" class="min-w-0 rounded-xl border border-storm/20 bg-deep/40 p-4">
        <div class="flex flex-wrap items-start justify-between gap-2">
          <label :for="'gaming-profile-' + client.uuid" class="min-w-0 break-words text-sm font-semibold text-silver">
            {{ deviceName(client) }}
          </label>
          <span v-if="dirty(client.uuid)" class="text-xs text-warning-bright">Unsaved change</span>
        </div>
        <p :id="'gaming-profile-current-' + client.uuid" class="mt-1 break-words text-xs text-storm">
          Currently opens: {{ profileName(assigned(client.uuid)) }}
        </p>
        <div class="mt-3 flex flex-col gap-2 sm:flex-row sm:items-center">
          <select :id="'gaming-profile-' + client.uuid" v-model="choices[client.uuid]"
                  class="focus-ring min-w-0 w-full rounded-lg border border-storm/30 bg-deep px-3 py-2.5 text-sm text-silver sm:flex-1"
                  :aria-describedby="'gaming-profile-current-' + client.uuid + ' gaming-profile-help-' + client.uuid"
                  :disabled="locked" @change="clearFeedback">
            <option value="">Standard streaming</option>
            <option v-for="profile in state.profiles" :key="profile.id" :value="profile.id"
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
    <div class="mt-4 flex flex-wrap items-center justify-between gap-3">
      <p v-if="state.enabled && devices.length" class="text-xs text-storm">Stop profile streams before changing assignments.</p>
      <button type="button" class="focus-ring rounded-lg px-1 py-2 text-sm text-ice disabled:opacity-40"
              :disabled="!!saving || loading" @click="refresh">
        {{ loading ? 'Refreshing…' : 'Refresh profiles' }}
      </button>
    </div>
  </section>
</template>

<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'

const props = defineProps({ clients: { type: Array, default: () => [] } })
const state = reactive({ enabled: false, available: false, changing: false, failed: false, profiles: [] })
const choices = reactive({})
const saving = ref(''), loading = ref(false)
const loadError = ref(''), actionError = ref(''), message = ref('')
const locked = computed(() => !!saving.value || loading.value || !!loadError.value || state.changing || state.failed || !state.available)
const eligible = client => !client.temporary_authorization && (Number(client.perm) & 0x04000000) !== 0
const assigned = id => state.profiles.find(profile => profile.clients.includes(id))?.id || ''
const profileName = id => state.profiles.find(profile => profile.id === id)?.name || 'Standard streaming'
const deviceName = client => client.friendly_name || client.name || 'Paired device'
const devices = computed(() => props.clients.filter(client => eligible(client) || assigned(client.uuid)))
const dirty = id => choices[id] !== assigned(id)

function clearFeedback() { message.value = ''; actionError.value = '' }

function selectionHelp(client) {
  if (!eligible(client)) return 'This device no longer has profile access. Choose Standard streaming to remove its assignment.'
  const selected = state.profiles.find(profile => profile.id === choices[client.uuid])
  if (!selected) return 'Uses the usual apps and account on this PC.'
  const others = selected.clients.filter(id => id !== client.uuid)
  if (!others.length) return 'Keeps this profile’s sign-ins, saves, and settings between sessions.'
  const names = others.map(id => {
    const device = props.clients.find(item => item.uuid === id)
    return device ? deviceName(device) : 'another paired device'
  })
  return 'Also assigned to ' + names.join(', ') + '. Only one of these devices can stream this profile at a time.'
}

function reconcileChoices(resetClient = '') {
  for (const id of Object.keys(choices)) {
    if (!props.clients.some(client => client.uuid === id)) delete choices[id]
  }
  for (const client of props.clients) {
    const choice = choices[client.uuid]
    if (client.uuid === resetClient || choice === undefined ||
        (choice !== '' && (!eligible(client) || !state.profiles.some(profile => profile.id === choice)))) {
      choices[client.uuid] = assigned(client.uuid)
    }
  }
}
watch(() => props.clients, () => reconcileChoices(), { deep: true })

function validSnapshot(next) {
  if (!next || ['enabled', 'available', 'changing', 'failed'].some(key => typeof next[key] !== 'boolean') ||
      !Array.isArray(next.profiles)) return false
  const profiles = new Set(), clients = new Set()
  for (const profile of next.profiles) {
    if (!profile || typeof profile.id !== 'string' || !profile.id || profiles.has(profile.id) ||
        typeof profile.name !== 'string' || !Array.isArray(profile.clients)) return false
    profiles.add(profile.id)
    for (const id of profile.clients) {
      if (typeof id !== 'string' || !id || clients.has(id)) return false
      clients.add(id)
    }
  }
  return true
}

async function loadProfiles(resetClient = '') {
  loading.value = true
  try {
    const response = await fetch('./api/multiseat/profiles', { credentials: 'include' })
    if (!response.ok) throw new Error('Could not load profile assignments. Refresh profiles to try again.')
    const next = await response.json()
    if (!validSnapshot(next)) throw new Error('Could not verify profile assignments. Refresh profiles to try again.')
    const edited = new Set(props.clients.filter(client => dirty(client.uuid) && choices[client.uuid] !== undefined).map(client => client.uuid))
    Object.assign(state, next)
    for (const client of props.clients) {
      if (!edited.has(client.uuid)) choices[client.uuid] = assigned(client.uuid)
    }
    reconcileChoices(resetClient)
    loadError.value = ''
    return true
  } catch (cause) {
    loadError.value = cause.message || 'Could not load profile assignments. Refresh profiles to try again.'
    return false
  } finally { loading.value = false }
}

async function refresh() {
  if (saving.value || loading.value) return
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
      message.value = 'Assignment saved. ' + name + ' now opens ' + profileName(requested) + '. Refresh the device library before starting a stream.'
    } else if (response.status === 202 || state.changing) {
      message.value = 'The assignment is still being applied. Refresh profiles to confirm it before starting a stream.'
    } else {
      actionError.value = 'The requested assignment could not be confirmed. Review the current assignment and try again.'
    }
  } catch (cause) {
    actionError.value = cause.message || 'Assignment was not saved.'
    await loadProfiles(client)
  } finally { saving.value = '' }
}
onMounted(refresh)
</script>
