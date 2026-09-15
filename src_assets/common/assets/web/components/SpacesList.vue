<template>
  <div>
    <p v-if="message" class="mt-3 text-sm text-silver" role="status">{{ message }}</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <div class="mt-4 grid gap-3 md:grid-cols-2 xl:grid-cols-3">
      <article v-for="space in active" :key="space.id" class="min-w-0 rounded-xl border border-storm/20 bg-deep/40 p-4">
        <div class="flex items-center gap-3">
          <span class="flex h-10 w-10 shrink-0 items-center justify-center rounded-full bg-ice/10 font-semibold text-ice" aria-hidden="true">{{ initials(space.name) }}</span>
          <div class="min-w-0">
            <h3 class="break-words font-semibold text-silver">{{ space.name }}</h3>
            <p class="mt-1 text-sm" :class="ready && activity && !spaceActivity(space).length ? 'text-success' : 'text-storm'" role="status">{{ activitySummary(space) }}</p>
          </div>
        </div>
        <p class="mt-3 text-xs text-storm">{{ space.steam ? 'Steam Account: Check Or Change In Steam Big Picture' : 'Gaming Space' }}</p>
        <p class="mt-3 break-words text-sm text-storm">{{ deviceSummary(space) }}</p>
        <div v-if="manageable" class="mt-4 flex flex-wrap gap-3">
          <button type="button" class="focus-ring rounded py-2 text-sm text-ice disabled:opacity-40" :disabled="locked"
                  :aria-label="'Rename ' + space.name" @click="open(space, 'rename')">Rename</button>
          <button type="button" class="focus-ring rounded py-2 text-sm text-storm disabled:opacity-40" :disabled="locked"
                  :aria-label="'Archive ' + space.name" @click="open(space, 'remove')">Archive Space</button>
        </div>
        <SpaceAccess v-if="accessAvailable" :space="space" :clients="clients" :locked="locked"
                     :ready="ready" :refresh="refresh" @busy="emit('busy', $event)" />
      </article>
    </div>
    <p v-if="!active.length" class="mt-3 text-sm text-storm">No active Spaces. Create one below, or restore an archived Space.</p>
    <p v-else class="mt-3 text-xs text-storm">One device can play in a Space at a time. Use separate Spaces for players who play together. Archiving keeps installed games and saves; it does not free storage.</p>
    <form v-if="selected" ref="panel" tabindex="-1" class="mt-4 rounded-xl border border-ice/30 bg-deep p-4"
          :aria-label="operation === 'rename' ? 'Rename Space' : operation === 'restore' ? 'Restore Space' : 'Archive Space'" @submit.prevent="submit">
      <h3 class="break-words font-semibold text-silver">{{ operation === 'rename' ? 'Rename ' : operation === 'restore' ? 'Restore ' : 'Archive ' }}{{ selected.name }}{{ operation === 'rename' ? '' : '?' }}</h3>
      <template v-if="operation === 'rename'">
        <label for="space-edit-name" class="mt-3 block text-sm text-silver">Space Name</label>
        <input id="space-edit-name" v-model="name" maxlength="128" autocomplete="off" :disabled="working"
               class="focus-ring mt-2 w-full rounded-lg border border-storm/30 bg-deep px-3 py-2.5 text-sm text-silver">
        <p class="mt-2 text-xs text-storm">Use a player or room name, such as Alex’s Space or Living Room. Renaming does not change the Steam account.</p>
      </template>
      <p v-else-if="operation === 'remove'" class="mt-3 text-sm text-storm">
        This hides the Space from your play list and removes its device access. Other allowed Spaces remain available; devices with none return to this PC’s desktop and apps.
        Installed games, saves, and Steam sign-in stay on this PC. Restore it from Archived Spaces whenever you need it.
        This does not free disk space.
      </p>
      <p v-else class="mt-3 text-sm text-storm">Your games, saves, and Steam sign-in will be available again. Choose which devices can use the space after restoring it.</p>
      <p class="mt-3 text-xs text-storm">Stop space streams before making this change.</p>
      <div class="mt-4 flex flex-wrap gap-3">
        <button type="submit" class="focus-ring rounded-lg border border-ice/30 px-3 py-2.5 text-sm text-ice disabled:opacity-40"
                :disabled="locked || working || (operation === 'rename' && !validName)">
          {{ working ? 'Saving…' : operation === 'rename' ? 'Save Name' : operation === 'restore' ? 'Restore Space' : 'Archive Space' }}
        </button>
        <button type="button" class="focus-ring rounded-lg px-3 py-2.5 text-sm text-storm" :disabled="working" @click="close">{{ submitted ? 'Close' : 'Cancel' }}</button>
      </div>
    </form>
    <details v-if="removed.length" class="mt-4 text-sm text-storm">
      <summary class="focus-ring cursor-pointer rounded py-2">Archived Spaces ({{ removed.length }})</summary>
      <p class="mt-2">These spaces keep their games and saves but cannot be opened from a device.</p>
      <div v-for="space in removed" :key="space.id" class="mt-3 flex flex-wrap items-center justify-between gap-2 rounded-lg border border-storm/20 p-3">
        <span class="min-w-0 break-words">{{ space.name }}</span>
        <button v-if="manageable" type="button" class="focus-ring rounded px-2 py-2 text-ice disabled:opacity-40"
                :disabled="locked" :aria-label="'Restore ' + space.name" @click="open(space, 'restore')">Restore</button>
      </div>
    </details>
  </div>
</template>

<script setup>
import { computed, nextTick, ref, watch } from 'vue'
import SpaceAccess from './SpaceAccess.vue'
const props = defineProps({ profiles: { type: Array, default: () => [] }, clients: { type: Array, default: () => [] },
  activity: { type: Array, default: null }, refreshing: Boolean,
  accessAvailable: Boolean, manageable: Boolean, locked: Boolean, ready: Boolean, refresh: { type: Function, required: true } })
const emit = defineEmits(['busy'])
const active = computed(() => props.profiles.filter(space => !space.archived))
const removed = computed(() => props.profiles.filter(space => space.archived))
const selected = ref(null), operation = ref(''), name = ref(''), working = ref(false), message = ref(''), error = ref(''), panel = ref(null)
let opener = null
const pending = ref(null), submitted = ref(false)
const validName = computed(() => !!name.value.trim() && new TextEncoder().encode(name.value.trim()).length <= 128 && !/[\u0000-\u001f\u007f]/u.test(name.value))
function deviceSummary(space) {
  const allowed = [...new Set([...space.clients, ...(space.access_clients || [])])]
  if (!allowed.length) return 'No device access yet. Choose devices under Device Access.'
  return 'Available To: ' + allowed.map(id => {
    const device = props.clients.find(client => client.uuid === id)
    return device?.friendly_name || device?.name || 'Paired device'
  }).join(', ')
}
function initials(name) { return name.trim().split(/\s+/u).slice(0, 2).map(word => [...word][0] || '').join('').toLocaleUpperCase() }
function spaceActivity(space) { return (props.activity || []).filter(item => item.profile_id === space.id) }
function activitySummary(space) {
  if (!props.activity) return props.refreshing ? 'Checking Status…' : 'Status Unavailable'
  const activity = spaceActivity(space)
  if (!activity.length) return props.ready ? 'Available' : 'Needs Attention'
  return activity.map(item => {
    const device = props.clients.find(client => client.uuid === item.client_id)
    const name = device?.friendly_name || device?.name || 'Paired Device'
    return (item.state === 'running' ? 'Playing On ' : item.state === 'starting' ? 'Starting On ' : 'Stopping On ') + name
  }).join(' · ')
}
async function open(space, action) {
  if (props.locked || !props.manageable) return
  opener = document.activeElement
  selected.value = { ...space }; operation.value = action; name.value = space.name
  error.value = ''; message.value = ''; submitted.value = false; pending.value = null
  await nextTick(); panel.value?.focus()
}
async function close() { selected.value = null; await nextTick(); if (opener?.isConnected) opener.focus() }
function confirmChange() {
  const request = pending.value
  if (!request || !props.ready) return false
  const current = props.profiles.find(space => space.id === request.profile_id)
  if (!current || !(request.operation === 'rename' ? current.name === request.name : current.archived === (request.operation === 'remove'))) return false
  error.value = ''
  message.value = request.operation === 'rename' ? 'Space renamed to ' + request.name + '. Refresh the library in Nova.' :
    request.operation === 'remove' ? request.previousName + ' was archived. Games and saves are kept in Archived Spaces.' :
      request.previousName + ' was restored. Choose its devices below.'
  pending.value = null
  close()
  return true
}
watch(() => [props.profiles, props.ready], () => { if (!working.value) confirmChange() })
async function submit() {
  if (props.locked || working.value || !selected.value || (operation.value === 'rename' && !validName.value)) return
  const request = { operation: operation.value, profile_id: selected.value.id }
  if (request.operation === 'rename') request.name = name.value.trim()
  pending.value = { ...request, previousName: selected.value.name }; submitted.value = true
  working.value = true; emit('busy', true); error.value = ''; message.value = ''
  try {
    const response = await fetch('./api/multiseat/profiles/manage', { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(request) })
    const result = await response.json()
    if (result?.profile_id !== request.profile_id || (response.status === 202 ? result.status !== false : !response.ok || result.status !== true))
      throw new Error(result?.message || 'The space change could not be confirmed. Refresh spaces before trying again.')
    message.value = 'Checking the saved change…'
  } catch (cause) { error.value = cause.message || 'Could not save the space change.' }
  try {
    const verified = await props.refresh()
    await nextTick()
    if (!(verified && confirmChange()) && !error.value) message.value = 'The change has not been confirmed yet. Refresh spaces to check its status.'
  } catch { error.value = 'Could not refresh spaces. Check the saved state before trying again.' }
  finally { working.value = false; emit('busy', false) }
}
</script>
