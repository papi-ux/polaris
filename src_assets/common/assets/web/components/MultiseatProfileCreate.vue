<template>
  <div class="mt-5 border-t border-storm/20 pt-4">
    <p v-if="message" class="mb-3 break-words text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mb-3 break-words text-sm text-warning-bright" role="alert">{{ error }}</p>
    <Button v-if="!showForm" ref="openButton" variant="outline" size="sm" :disabled="locked || !sources.length" @click="openForm">
      {{ $t('spaces.create') }}
    </Button>
    <p v-if="!sources.length" class="mt-2 text-xs text-storm">{{ $t('spaces.create_first_hint') }}</p>
    <form v-if="showForm" class="rounded-xl border border-storm/20 bg-deep/40 p-4" @submit.prevent="submit">
      <h3 class="text-sm font-semibold text-silver">{{ $t('spaces.new_space') }}</h3>
      <p class="mt-1 text-sm text-storm">{{ $t('spaces.new_space_copy') }}</p>
      <label for="new-steam-profile-name" class="mt-4 block text-sm text-silver">{{ $t('spaces.who_for') }}</label>
      <input id="new-steam-profile-name" ref="nameInput" v-model="name" type="text" autocomplete="off" maxlength="128"
             class="settings-input mt-2 text-sm" :placeholder="$t('spaces.name_placeholder')"
             :disabled="locked || !!pending" aria-describedby="new-steam-name-help">
      <p id="new-steam-name-help" class="mt-1 text-xs text-storm">
        {{ name.trim() && !validName ? $t('spaces.name_invalid') : $t('spaces.name_help') }}
      </p>
      <details v-if="sources.length > 1" class="settings-disclosure mt-4">
        <summary class="settings-disclosure-summary focus-ring cursor-pointer rounded text-sm text-storm">
          <span>{{ $t('spaces.advanced') }}</span>
          <svg class="settings-disclosure-chevron h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7" />
          </svg>
        </summary>
        <label for="new-steam-profile-source" class="mt-4 block text-sm text-silver">{{ $t('spaces.based_on') }}</label>
        <select id="new-steam-profile-source" v-model="source" :disabled="locked || !!pending" class="settings-input mt-2 min-w-0 text-sm">
          <option v-for="profile in sources" :key="profile.id" :value="profile.id">{{ profile.name }}</option>
        </select>
      </details>
      <p class="mt-3 text-xs text-storm">{{ $t('spaces.create_note') }}</p>
      <div class="mt-4 flex flex-wrap gap-2">
        <Button type="submit" variant="outline" size="sm" :loading="working"
                :disabled="locked || working || (!pending && (!validName || !validSource))">
          {{ working ? $t('spaces.creating') : pending ? $t('spaces.retry_creation') : $t('spaces.create_submit') }}
        </Button>
        <Button v-if="pending" type="button" variant="ghost" size="sm" class="text-ice" :disabled="working || refreshing" @click="checkStatus">
          {{ $t('spaces.check_creation') }}
        </Button>
        <Button v-else type="button" variant="ghost" size="sm" :disabled="working" @click="closeForm">{{ $t('spaces.cancel') }}</Button>
      </div>
      <p v-if="pending" class="mt-3 text-xs text-storm">
        {{ $t('spaces.retry_note') }}
        {{ requestSaved ? $t('spaces.retry_saved') : $t('spaces.retry_unsaved') }}
      </p>
    </form>
  </div>
</template>

<script setup>
import { computed, inject, nextTick, onMounted, ref, watch } from 'vue'
import Button from './Button.vue'
import { useToast } from '../composables/useToast.js'

const props = defineProps({
  profiles: { type: Array, default: () => [] },
  locked: Boolean, ready: Boolean, refreshing: Boolean,
  refresh: { type: Function, required: true },
})
const emit = defineEmits(['busy'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const { toast } = useToast()
// A new Space copies the runtime setup of a live Steam Space; archived ones are
// not offered, since their catalog row carries no devices to inherit from.
const sources = computed(() => props.profiles.filter(profile => profile.steam === true && !profile.archived))
const name = ref(''), source = ref(''), showForm = ref(false), working = ref(false)
const message = ref(''), error = ref(''), pending = ref(null)
const requestSaved = ref(false)
const pendingKey = 'polaris:spaces:create-request:v1'
const nameInput = ref(null), openButton = ref(null)
const validName = computed(() => !!name.value.trim() && new TextEncoder().encode(name.value.trim()).length <= 128 &&
  !/[\u0000-\u001f\u007f]/u.test(name.value.trim()))
const validSource = computed(() => sources.value.some(profile => profile.id === source.value))

function savePending() {
  try {
    sessionStorage.setItem(pendingKey, JSON.stringify(pending.value))
    requestSaved.value = true
  } catch { requestSaved.value = false }
}
function clearPending() {
  try {
    const saved = JSON.parse(sessionStorage.getItem(pendingKey) || 'null')
    // A result arriving after navigation must not erase a newer request.
    if (saved?.request_id === pending.value?.request_id) sessionStorage.removeItem(pendingKey)
  } catch { /* The in-memory request remains sufficient to verify this result. */ }
  pending.value = null; requestSaved.value = false
}
function restorePending() {
  try {
    const raw = sessionStorage.getItem(pendingKey)
    if (!raw || raw.length > 4096) return
    const saved = JSON.parse(raw)
    if (!saved || typeof saved.request_id !== 'string' ||
        !/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/u.test(saved.request_id) ||
        typeof saved.source_profile_id !== 'string' || !saved.source_profile_id || saved.source_profile_id.length > 128 ||
        typeof saved.name !== 'string' || !saved.name.trim() ||
        new TextEncoder().encode(saved.name).length > 128 || /[\u0000-\u001f\u007f]/u.test(saved.name)) return
    pending.value = { request_id: saved.request_id, source_profile_id: saved.source_profile_id, name: saved.name }
    name.value = saved.name; source.value = saved.source_profile_id
    showForm.value = true; requestSaved.value = true
    message.value = t('spaces.creation_waiting')
    confirmCreation()
  } catch { /* Storage may be disabled. Never submit an unverified saved request. */ }
}
onMounted(restorePending)

async function openForm() {
  source.value = sources.value[0]?.id || ''
  showForm.value = true
  message.value = ''; error.value = ''
  await nextTick(); nameInput.value?.focus()
}
async function closeForm() {
  showForm.value = false
  await nextTick(); openButton.value?.$el?.focus?.()
}
function confirmCreation() {
  if (!pending.value || !props.ready) return false
  const found = props.profiles.find(profile => profile.id === pending.value.request_id &&
    profile.name === pending.value.name && profile.steam === true && !profile.archived)
  if (!found) return false
  message.value = t('spaces.created', { name: found.name })
  toast(message.value, 'success')
  error.value = ''; clearPending(); name.value = ''
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
    error.value = t('spaces.creation_refresh_failed')
    return false
  }
}

async function checkStatus() {
  if (working.value || props.refreshing) return
  const verified = await refreshProfiles()
  await nextTick()
  if (verified && !confirmCreation() && pending.value && props.ready) {
    message.value = t('spaces.creation_unconfirmed')
  }
}

async function submit() {
  if (props.locked || working.value || (!pending.value && (!validName.value || !validSource.value))) return
  working.value = true; emit('busy', true)
  message.value = ''; error.value = ''
  try {
    if (!pending.value) {
      pending.value = { request_id: crypto.randomUUID(), source_profile_id: source.value, name: name.value.trim() }
      savePending()
    }
    const response = await fetch('./api/multiseat/profiles', {
      credentials: 'include', method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(pending.value),
    })
    const result = await response.json()
    if (!result || typeof result !== 'object') throw new Error(t('spaces.creation_unverified'))
    if (response.status !== 202 && (!response.ok || result.status !== true)) {
      // These refusals happen before provisioning; the form can be corrected.
      if (response.status === 400 || response.status === 404) clearPending()
      throw new Error(result.message || result.error || t('spaces.creation_failed'))
    }
    if (result.profile_id !== pending.value.request_id ||
        (response.status === 202 ? result.status !== false : result.status !== true)) {
      throw new Error(t('spaces.creation_unverified'))
    }
    message.value = response.status === 202 ? t('spaces.creation_pending') : t('spaces.creation_checking')
  } catch (cause) {
    error.value = cause.message || t('spaces.creation_error')
  } finally {
    const verified = await refreshProfiles()
    await nextTick()
    if (verified && !confirmCreation() && pending.value && props.ready && !error.value) {
      message.value = t('spaces.creation_unconfirmed')
    }
    working.value = false; emit('busy', false)
  }
}
</script>
