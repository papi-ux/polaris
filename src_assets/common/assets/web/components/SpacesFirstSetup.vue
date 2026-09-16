<template>
  <div class="mt-5 border-t border-storm/20 pt-5" aria-labelledby="spaces-first-title">
    <h3 id="spaces-first-title" class="text-base font-semibold text-silver">Set up your first space</h3>
    <p class="mt-2 max-w-2xl text-sm text-storm">
      Give your space a name. Polaris will download its gaming runtime and prepare a separate Steam home for your sign-in, games and saves.
    </p>
    <p class="mt-2 text-sm text-storm">Prepare a Steam home, choose its graphics card, then enable Spaces and assign your device.</p>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <p v-if="snapshot?.message" class="mt-3 text-sm text-storm">{{ snapshot.message }}</p>
    <div v-if="snapshot?.job" class="mt-4 rounded-xl border border-storm/20 bg-deep/40 p-4">
      <h4 class="font-medium text-silver">{{ snapshot.job.name }}</h4>
      <p class="mt-2 text-sm text-silver" role="status" aria-live="polite">{{ snapshot.job.message }}</p>
      <p v-if="snapshot.job.state === 'prepared'" class="mt-2 text-sm text-storm">Your Steam home is saved. Setup has not started a game or enabled streaming.</p>
      <div v-if="snapshot.job.can_activate" class="mt-4 space-y-3">
        <label for="spaces-first-gpu" class="block text-sm font-medium text-silver">Graphics card</label>
        <select id="spaces-first-gpu" v-model="gpuId" :disabled="busy || !connected || !!snapshot.job.gpu_id"
                class="focus-ring w-full rounded-lg border border-storm/30 bg-void px-3 py-2.5 text-silver">
          <option v-for="gpu in snapshot.graphics" :key="gpu.id" :value="gpu.id">{{ gpu.label }}</option>
        </select>
        <p v-if="!snapshot.graphics?.length" class="text-sm text-storm">No accessible graphics card matches this runtime. Recheck host setup before continuing.</p>
        <p class="text-sm text-storm">This preview sets up one Space at a time. Guided setup for simultaneous play is still being added.</p>
        <button type="button" class="job-button" :disabled="busy || !connected || !hostReady || !gpuId || !snapshot.graphics?.some(g => g.id === gpuId)"
                @click="send({ operation: 'activate', request_id: snapshot.job.request_id, gpu_id: gpuId })">
          {{ snapshot.job.state === 'activation_failed' ? 'Retry configuration' : 'Enable Spaces' }}
        </button>
      </div>
      <div v-if="snapshot.job.state === 'restart_required'" class="mt-4 space-y-3">
        <p class="text-sm text-storm">Save your game before restarting. Restarting Polaris disconnects active streams. After reconnecting, assign your Nova device to this Space below and open it in Nova to sign in to Steam.</p>
        <button type="button" class="job-button" :disabled="busy || !connected || restarting" @click="restart">
          {{ restarting ? 'Restart requested…' : 'Restart Polaris and finish setup' }}
        </button>
      </div>
      <div class="mt-3 flex flex-wrap gap-3">
        <button v-if="snapshot.job.can_cancel" type="button" :disabled="busy || !connected" class="job-button"
                @click="send({ operation: 'cancel', request_id: snapshot.job.request_id })">Stop setup</button>
        <button v-if="snapshot.job.can_retry" type="button" :disabled="busy || !connected || !hostReady" class="job-button"
                @click="send(requestForJob(snapshot.job))">Retry setup</button>
      </div>
    </div>
    <div v-else-if="pending" class="mt-4 text-sm text-storm">
      <p>A request for “{{ pending.name }}” was saved in this browser. Reconnect to check it, then retry the same request if needed.</p>
      <button type="button" class="job-button mt-3" :disabled="busy || !connected || !snapshot?.available || !hostReady"
              @click="send(pending)">Retry saved request</button>
    </div>
    <form v-else-if="snapshot?.available" class="mt-4 max-w-xl space-y-3" @submit.prevent="start">
      <div>
        <label for="spaces-first-name" class="block text-sm font-medium text-silver">Who Is This Space For?</label>
        <input id="spaces-first-name" v-model="name" type="text" maxlength="128" autocomplete="off" placeholder="e.g. Alex’s Space"
               :disabled="busy || !connected" class="focus-ring mt-2 w-full rounded-lg border border-storm/30 bg-void px-3 py-2.5 text-silver" />
      </div>
      <div v-if="snapshot.runtimes.length > 1">
        <label for="spaces-first-runtime" class="block text-sm font-medium text-silver">Gaming runtime</label>
        <select id="spaces-first-runtime" v-model="runtimeId" :disabled="busy || !connected"
                class="focus-ring mt-2 w-full rounded-lg border border-storm/30 bg-void px-3 py-2.5 text-silver">
          <option v-for="runtime in snapshot.runtimes" :key="runtime.id" :value="runtime.id">{{ runtimeLabel(runtime) }}</option>
        </select>
      </div>
      <p v-else class="text-sm text-storm">{{ runtimeLabel(snapshot.runtimes[0]) }}</p>
      <p class="text-xs text-storm">The download can be several gigabytes. You can leave this page while it runs.</p>
      <p v-if="!hostReady" class="text-sm text-storm">Complete the host setup checks above before starting or retrying.</p>
      <button type="submit" class="job-button" :disabled="busy || !connected || !hostReady || !name.trim()">Download and prepare</button>
    </form>
    <button type="button" class="focus-ring mt-3 rounded px-1 py-2 text-sm text-ice disabled:opacity-40"
            :disabled="busy" @click="refresh">{{ busy ? 'Checking setup…' : 'Reconnect to setup' }}</button>
  </div>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import { requestForJob, validJobSnapshot, validSetupStart } from '../spaces-job.js'

defineProps({ hostReady: { type: Boolean, default: false } })
const snapshot = ref(null), busy = ref(false), connected = ref(false), error = ref('')
const name = ref(''), runtimeId = ref(''), gpuId = ref(''), pending = ref(null), restarting = ref(false)
const storageKey = 'polaris.spaces.first-setup'
try {
  const saved = JSON.parse(sessionStorage.getItem(storageKey) || 'null')
  if (validSetupStart(saved)) pending.value = saved
} catch { /* The host remains the authority if browser storage is unavailable. */ }
let request, poll, disposed = false
const runtimeLabel = runtime => runtime?.variant === 'nvidia'
  ? 'Steam for NVIDIA driver ' + runtime.nvidia_driver : 'Steam for AMD and Intel graphics'

function adopt(next) {
  snapshot.value = next; connected.value = true
  if (next.job?.gpu_id) gpuId.value = next.job.gpu_id
  else if (!next.graphics?.some(g => g.id === gpuId.value)) gpuId.value = next.graphics?.[0]?.id || ''
  if (!next.runtimes.some(runtime => runtime.id === runtimeId.value)) runtimeId.value = next.runtimes[0]?.id || ''
  if (next.job) {
    pending.value = null
    try { sessionStorage.removeItem(storageKey) } catch { /* Server job is durable. */ }
  }
}
async function exchange(action) {
  if (busy.value || disposed) return
  clearTimeout(poll)
  busy.value = true; connected.value = false; error.value = ''
  request = new AbortController()
  const timeout = setTimeout(() => request.abort(), 12000)
  try {
    const response = await fetch('./api/spaces/setup/job', {
      credentials: 'include', cache: 'no-store', signal: request.signal,
      ...(action ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(action) } : {}),
    })
    if (disposed) return
    if (![200, 202, 409, 503].includes(response.status)) throw new Error(response.status === 404
      ? 'This host does not provide guided first-space preparation yet.'
      : 'Could not reach setup. Check your connection and sign in again if needed.')
    const next = await response.json()
    if (disposed) return
    if (!validJobSnapshot(next)) throw new Error('The saved setup response could not be verified. Reconnect before continuing.')
    adopt(next)
    if (action && (!response.ok || next.accepted !== true))
      error.value = 'This request was not accepted. Check the saved setup shown here before retrying.'
  } catch (cause) {
    if (!disposed) error.value = cause.name === 'AbortError'
      ? 'The connection timed out. Setup may still be running on the host. Reconnect to check before retrying.'
      : cause.message || 'Could not check setup. Reconnect to try again.'
  } finally {
    clearTimeout(timeout); busy.value = false
    if (!disposed && ['downloading', 'preparing', 'configuring'].includes(snapshot.value?.job?.state))
      poll = setTimeout(refresh, 2000)
  }
}
async function restart() {
  if (restarting.value || busy.value || !connected.value || snapshot.value?.job?.state !== 'restart_required') return
  restarting.value = true; error.value = ''
  request = new AbortController()
  const timeout = setTimeout(() => request.abort(), 12000)
  try {
    const response = await fetch('./api/restart', { method: 'POST', credentials: 'include', signal: request.signal,
      headers: { 'Content-Type': 'application/json' }, body: '{}' })
    if (!response.ok || (await response.json()).restarting !== true) throw new Error('Restart was not accepted.')
    if (!disposed) error.value = 'Restart requested. Reconnect to Polaris, then return to Spaces to assign your device.'
  } catch {
    if (!disposed) { connected.value = false; error.value = 'The restart response could not be confirmed. Reconnect to Polaris before trying again.' }
  } finally { clearTimeout(timeout); restarting.value = false }
}
const refresh = () => exchange()
const send = action => exchange(action)
function start() {
  let action
  try { action = { operation: 'start', request_id: crypto.randomUUID(), runtime_id: runtimeId.value, name: name.value.trim() } }
  catch { error.value = 'Use a secure connection to Polaris before preparing a space.'; return }
  if (!validSetupStart(action)) { error.value = 'Use a space name of up to 128 bytes without control characters.'; return }
  pending.value = action
  try { sessionStorage.setItem(storageKey, JSON.stringify(action)) } catch { /* Retain in memory until the host confirms. */ }
  send(action)
}
onMounted(refresh)
onUnmounted(() => { disposed = true; clearTimeout(poll); request?.abort() })
</script>

<style scoped>
.job-button {
  border: 1px solid rgb(136 192 208 / 0.3);
  border-radius: 0.5rem;
  padding: 0.625rem 0.75rem;
  font-size: 0.875rem;
  color: var(--color-ice, #88c0d0);
}
.job-button:focus-visible { outline: 2px solid currentColor; outline-offset: 2px; }
.job-button:disabled { opacity: 0.4; }
</style>
