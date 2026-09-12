<template>
  <section v-if="state.enabled" class="section-card" aria-labelledby="profile-assignment-title">
    <h2 id="profile-assignment-title" class="section-title">Separate gaming profiles</h2>
    <p class="mt-2 text-sm text-storm">
      Choose which profile each device opens. Devices on the same profile share its games and settings,
      with one stream at a time. Choose Standard streaming to use the existing host apps.
    </p>
    <p class="mt-2 text-sm text-storm">Stop profile streams before changing assignments.</p>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <p v-if="state.failed" class="mt-3 text-sm text-warning-bright" role="alert">
      Profile settings could not be restored. Review the configuration and restart Polaris.
    </p>
    <div v-for="client in devices" :key="client.uuid" class="mt-4 flex flex-wrap items-center gap-3">
      <label :for="'gaming-profile-' + client.uuid" class="min-w-40 text-sm text-silver">
        {{ client.friendly_name || client.name }}
      </label>
      <select :id="'gaming-profile-' + client.uuid" v-model="choices[client.uuid]"
              class="focus-ring rounded-lg border border-storm/30 bg-deep px-3 py-2 text-sm text-silver"
              :disabled="locked" @change="message = ''">
        <option value="">Standard streaming</option>
        <option v-for="profile in state.profiles" :key="profile.id" :value="profile.id"
                :disabled="!eligible(client)">{{ profile.name }}</option>
      </select>
      <button type="button" class="focus-ring rounded-lg border border-ice/30 px-3 py-2 text-sm text-ice"
              :disabled="locked || choices[client.uuid] === assigned(client.uuid)"
              @click="save(client.uuid)">Save assignment</button>
    </div>
    <button type="button" class="focus-ring mt-4 text-sm text-ice" :disabled="saving" @click="refresh">Refresh profiles</button>
  </section>
</template>

<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'

const props = defineProps({ clients: { type: Array, default: () => [] } })
const state = reactive({ enabled: false, available: false, changing: false, failed: false, profiles: [] })
const choices = reactive({})
const saving = ref(false), error = ref(''), message = ref('')
const locked = computed(() => saving.value || state.changing || state.failed || !state.available)
const eligible = client => !client.temporary_authorization && (Number(client.perm) & 0x04000000) !== 0
const assigned = id => state.profiles.find(profile => profile.clients.includes(id))?.id || ''
const devices = computed(() => props.clients.filter(client => eligible(client) || assigned(client.uuid)))

function resetChoices() {
  for (const client of props.clients) choices[client.uuid] = assigned(client.uuid)
}
watch(() => props.clients.map(client => client.uuid).join('|'), resetChoices)

async function refresh() {
  try {
    const response = await fetch('./api/multiseat/profiles', { credentials: 'include' })
    if (!response.ok) throw new Error('Could not load profile assignments.')
    const next = await response.json()
    if (typeof next.enabled !== 'boolean' || !Array.isArray(next.profiles) ||
        next.profiles.some(p => typeof p.id !== 'string' || typeof p.name !== 'string' || !Array.isArray(p.clients))) {
      throw new Error('Profile assignments returned an invalid response.')
    }
    Object.assign(state, next)
    resetChoices()
  } catch (cause) { error.value = cause.message }
}

async function save(client) {
  if (locked.value) return
  saving.value = true; error.value = ''; message.value = ''
  try {
    const response = await fetch('./api/multiseat/assign', {
      credentials: 'include', method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ client_id: client, profile_id: choices[client] }),
    })
    const result = await response.json()
    if (response.status === 202) message.value = result.message
    else if (!response.ok || result.status !== true) throw new Error(result.message || result.error || 'Assignment was not saved.')
    else message.value = 'Assignment saved. Refresh the device library before starting a stream.'
  } catch (cause) { error.value = cause.message }
  finally { await refresh(); saving.value = false }
}
onMounted(refresh)
</script>
