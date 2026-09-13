<template>
  <div class="mt-5 border-t border-storm/20 pt-4">
    <p v-if="message" class="mb-3 break-words text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mb-3 break-words text-sm text-warning-bright" role="alert">{{ error }}</p>
    <button v-if="!showForm" ref="openButton" type="button"
            class="focus-ring rounded-lg border border-ice/30 px-3 py-2.5 text-sm text-ice disabled:opacity-40"
            :disabled="locked || !sources.length" @click="openForm">Create Steam profile</button>
    <p v-if="!sources.length" class="mt-2 text-xs text-storm">
      Set up the first Steam profile on this PC before creating more profiles here.
    </p>
    <form v-if="showForm" class="rounded-xl border border-storm/20 bg-deep/40 p-4" @submit.prevent="submit">
      <h3 class="text-sm font-semibold text-silver">New Steam profile</h3>
      <p class="mt-1 text-sm text-storm">
        Starts with its own Steam sign-in, saves, and settings. After creating it, assign a device and open Big Picture to sign in.
      </p>
      <label for="new-steam-profile-name" class="mt-4 block text-sm text-silver">Profile name</label>
      <input id="new-steam-profile-name" ref="nameInput" v-model="name" type="text" autocomplete="off" maxlength="128"
             class="focus-ring mt-2 w-full rounded-lg border border-storm/30 bg-deep px-3 py-2.5 text-sm text-silver"
             placeholder="e.g. Player 2" :disabled="locked || !!pending" aria-describedby="new-steam-name-help">
      <p id="new-steam-name-help" class="mt-1 text-xs text-storm">
        {{ name.trim() && !validName ? 'Use a shorter name without control characters.' : 'Choose a name you will recognize when assigning devices.' }}
      </p>
      <template v-if="sources.length > 1">
        <label for="new-steam-profile-source" class="mt-4 block text-sm text-silver">Use Steam setup from</label>
        <select id="new-steam-profile-source" v-model="source" :disabled="locked || !!pending"
                class="focus-ring mt-2 w-full min-w-0 rounded-lg border border-storm/30 bg-deep px-3 py-2.5 text-sm text-silver">
          <option v-for="profile in sources" :key="profile.id" :value="profile.id">{{ profile.name }}</option>
        </select>
      </template>
      <p class="mt-3 text-xs text-storm">Stop profile streams before creating a profile. Creation can take a little while.</p>
      <div class="mt-4 flex flex-wrap gap-3">
        <button type="submit" class="focus-ring rounded-lg border border-ice/30 px-3 py-2.5 text-sm text-ice disabled:opacity-40"
                :disabled="locked || working || (!pending && (!validName || !validSource))">
          {{ working ? 'Creating…' : pending ? 'Retry creation' : 'Create profile' }}
        </button>
        <button v-if="pending" type="button" class="focus-ring rounded-lg px-3 py-2.5 text-sm text-ice disabled:opacity-40"
                :disabled="working || refreshing" @click="checkStatus">Check creation status</button>
        <button v-else type="button" class="focus-ring rounded-lg px-3 py-2.5 text-sm text-storm"
                :disabled="working" @click="closeForm">Cancel</button>
      </div>
      <p v-if="pending" class="mt-3 text-xs text-storm">
        Retrying checks the same profile request. Keep this form open until its result is confirmed.
      </p>
    </form>
  </div>
</template>

<script setup>
import { computed, nextTick, ref, watch } from 'vue'

const props = defineProps({
  profiles: { type: Array, default: () => [] },
  locked: Boolean, ready: Boolean, refreshing: Boolean,
  refresh: { type: Function, required: true },
})
const emit = defineEmits(['busy'])
const sources = computed(() => props.profiles.filter(profile => profile.steam === true))
const name = ref(''), source = ref(''), showForm = ref(false), working = ref(false)
const message = ref(''), error = ref(''), pending = ref(null)
const nameInput = ref(null), openButton = ref(null)
const validName = computed(() => !!name.value.trim() && new TextEncoder().encode(name.value.trim()).length <= 128 &&
  !/[\u0000-\u001f\u007f]/u.test(name.value.trim()))
const validSource = computed(() => sources.value.some(profile => profile.id === source.value))

async function openForm() {
  source.value = sources.value[0]?.id || ''
  showForm.value = true
  message.value = ''; error.value = ''
  await nextTick(); nameInput.value?.focus()
}
async function closeForm() {
  showForm.value = false
  await nextTick(); openButton.value?.focus()
}
function confirmCreation() {
  if (!pending.value || !props.ready) return false
  const found = props.profiles.find(profile => profile.id === pending.value.request_id &&
    profile.name === pending.value.name && profile.steam === true)
  if (!found) return false
  message.value = found.name + ' was created. Assign it to a device below, then open Big Picture to sign in.'
  error.value = ''; pending.value = null; name.value = ''
  closeForm()
  return true
}
watch(() => [props.profiles, props.ready], () => { if (!working.value) confirmCreation() })
watch(sources, () => {
  if (!pending.value && !validSource.value) source.value = sources.value[0]?.id || ''
})

async function refreshProfiles() {
  try { return await props.refresh() }
  catch {
    error.value = 'Could not refresh profiles. Check creation status before retrying.'
    return false
  }
}

async function checkStatus() {
  if (working.value || props.refreshing) return
  const verified = await refreshProfiles()
  await nextTick()
  if (verified && !confirmCreation() && pending.value && props.ready) {
    message.value = 'The profile has not been confirmed yet. Retry creation to check the same request.'
  }
}

async function submit() {
  if (props.locked || working.value || (!pending.value && (!validName.value || !validSource.value))) return
  working.value = true; emit('busy', true)
  message.value = ''; error.value = ''
  try {
    if (!pending.value) pending.value = {
      request_id: crypto.randomUUID(), source_profile_id: source.value, name: name.value.trim(),
    }
    const response = await fetch('./api/multiseat/profiles', {
      credentials: 'include', method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(pending.value),
    })
    const result = await response.json()
    if (!result || typeof result !== 'object') throw new Error('The creation response could not be verified. Check its status before retrying.')
    if (response.status !== 202 && (!response.ok || result.status !== true)) {
      // These refusals happen before provisioning; the form can be corrected.
      if (response.status === 400 || response.status === 404) pending.value = null
      throw new Error(result.message || result.error || 'The profile could not be created. Check its status before retrying.')
    }
    if (result.profile_id !== pending.value.request_id ||
        (response.status === 202 ? result.status !== false : result.status !== true)) {
      throw new Error('The creation response could not be verified. Check its status before retrying.')
    }
    message.value = response.status === 202 ?
      'Polaris is still creating the profile. Check creation status in a moment.' : 'Checking the saved profile…'
  } catch (cause) {
    error.value = cause.message || 'Creation could not be confirmed. Check its status before retrying.'
  } finally {
    const verified = await refreshProfiles()
    await nextTick()
    if (verified && !confirmCreation() && pending.value && props.ready && !error.value) {
      message.value = 'The profile has not been confirmed yet. Retry creation to check the same request.'
    }
    working.value = false; emit('busy', false)
  }
}
</script>
