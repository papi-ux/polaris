<template>
  <div class="mt-5 border-t border-storm/20 pt-5" role="group" aria-labelledby="spaces-first-title">
    <p class="section-kicker">{{ $t('spaces.kicker') }}</p>
    <h3 id="spaces-first-title" class="text-base font-semibold text-silver">{{ $t('spaces.first_title') }}</h3>
    <template v-if="!runtimeNotPublished">
      <p class="mt-2 max-w-2xl text-sm text-storm">{{ $t('spaces.first_copy') }}</p>
      <p class="mt-2 text-sm text-storm">{{ $t('spaces.first_steps') }}</p>
    </template>
    <p v-if="error" class="mt-3 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <p v-if="notice" class="mt-3 text-sm text-silver" role="status">{{ notice }}</p>
    <div v-if="snapshot && !snapshot.available && !snapshot.job" class="mt-4 rounded-xl border border-storm/20 bg-deep/40 p-4" data-setup-unavailable>
      <h4 class="font-medium text-silver">{{ $t('spaces.unavailable_title') }}</h4>
      <p class="mt-2 text-sm text-storm">{{ unavailableCopy }}</p>
      <a v-if="unavailableAnchor" :href="docsUrl + unavailableAnchor" target="_blank" rel="noopener noreferrer"
         class="focus-ring mt-3 inline-block rounded py-2 text-sm text-ice hover:underline">{{ unavailableLinkLabel }}</a>
    </div>
    <p v-else-if="snapshot?.message && !snapshot.job" class="mt-3 text-sm text-storm">{{ snapshot.message }}</p>
    <div v-if="snapshot?.job" class="mt-4 rounded-xl border border-storm/20 bg-deep/40 p-4" data-setup-job>
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h4 class="font-medium text-silver">{{ snapshot.job.name }}</h4>
        <StatusBadge :status="jobTone" :label="jobLabel" />
      </div>
      <p class="mt-2 text-sm text-silver" role="status" aria-live="polite">{{ snapshot.job.message }}</p>
      <p v-if="snapshot.job.state === 'prepared'" class="mt-2 text-sm text-storm">{{ $t('spaces.prepared_note') }}</p>
      <ul v-if="blockedBy.length" class="mt-3 space-y-1 text-sm text-warning-bright" data-setup-blocked>
        <li v-for="reason in blockedBy" :key="reason">{{ blockedCopy(reason) }}</li>
      </ul>
      <div v-if="snapshot.job.recovery" class="mt-3 rounded-lg border border-warning/30 bg-warning/5 p-3 text-sm" data-setup-recovery>
        <p class="font-semibold text-warning-bright">{{ $t('spaces.recovery_title') }}</p>
        <p class="mt-1 text-storm">{{ $t('spaces.recovery_copy') }}</p>
        <dl class="mt-2 grid gap-1 text-xs text-storm">
          <div v-if="snapshot.job.recovery.reference" class="flex flex-wrap gap-2"><dt class="font-semibold">{{ $t('spaces.recovery_reference') }}</dt><dd class="break-all font-mono">{{ snapshot.job.recovery.reference }}</dd></div>
          <div v-if="snapshot.job.recovery.image" class="flex flex-wrap gap-2"><dt class="font-semibold">{{ $t('spaces.recovery_image') }}</dt><dd class="break-all font-mono">{{ snapshot.job.recovery.image }}</dd></div>
          <div v-if="snapshot.job.recovery.code" class="flex flex-wrap gap-2"><dt class="font-semibold">{{ $t('spaces.recovery_code') }}</dt><dd class="font-mono">{{ snapshot.job.recovery.code }}</dd></div>
        </dl>
        <a :href="docsUrl + snapshot.job.recovery.doc_anchor" target="_blank" rel="noopener noreferrer"
           class="focus-ring mt-2 inline-block rounded py-1 text-ice hover:underline">{{ $t('spaces.recovery_guide') }}</a>
      </div>
      <div v-if="snapshot.job.can_activate" class="mt-4 space-y-3">
        <label for="spaces-first-gpu" class="block text-sm font-medium text-silver">{{ $t('spaces.graphics_card') }}</label>
        <select id="spaces-first-gpu" v-model="gpuId" :disabled="busy || !connected || !!snapshot.job.gpu_id" class="settings-input text-sm">
          <option v-for="gpu in snapshot.graphics" :key="gpu.id" :value="gpu.id">{{ gpu.label }}</option>
        </select>
        <p v-if="!snapshot.graphics?.length" class="text-sm text-storm">{{ $t('spaces.no_gpu') }}</p>
        <p class="text-sm text-storm">{{ $t('spaces.one_at_a_time') }}</p>
        <Button variant="outline" size="sm" :disabled="busy || !connected || !hostReady || !gpuId || !snapshot.graphics?.some(g => g.id === gpuId)"
                @click="send({ operation: 'activate', request_id: snapshot.job.request_id, gpu_id: gpuId })">
          {{ snapshot.job.state === 'activation_failed' ? $t('spaces.retry_configuration') : $t('spaces.enable') }}
        </Button>
      </div>
      <div v-if="snapshot.job.state === 'restart_required'" class="mt-4 space-y-3">
        <p class="text-sm text-storm">{{ $t('spaces.restart_copy') }}</p>
        <Button variant="outline" size="sm" :disabled="busy || !connected || restarting || restartRequested" data-setup-restart @click="restartOpen = true">
          {{ restarting ? $t('spaces.restart_pending') : $t('spaces.restart') }}
        </Button>
      </div>
      <div class="mt-3 flex flex-wrap gap-2">
        <Button v-if="snapshot.job.can_cancel" variant="ghost" size="sm" class="text-warning-bright" :disabled="busy || !connected"
                @click="send({ operation: 'cancel', request_id: snapshot.job.request_id })">{{ $t('spaces.stop_setup') }}</Button>
        <Button v-if="snapshot.job.can_retry" variant="outline" size="sm" :disabled="busy || !connected || !hostReady"
                @click="send(requestForJob(snapshot.job))">{{ $t('spaces.retry_setup') }}</Button>
      </div>
    </div>
    <div v-else-if="pending" class="mt-4 text-sm text-storm">
      <p>{{ $t('spaces.saved_request', { name: pending.name }) }}</p>
      <Button variant="outline" size="sm" class="mt-3" :disabled="busy || !connected || !snapshot?.available || !hostReady"
              @click="send(pending)">{{ $t('spaces.retry_saved_request') }}</Button>
    </div>
    <form v-else-if="snapshot?.available" class="mt-4 max-w-xl space-y-3" @submit.prevent="start">
      <div>
        <label for="spaces-first-name" class="block text-sm font-medium text-silver">{{ $t('spaces.who_for') }}</label>
        <input id="spaces-first-name" v-model="name" type="text" maxlength="128" autocomplete="off" :placeholder="$t('spaces.name_placeholder')"
               :disabled="busy || !connected" class="settings-input mt-2 text-sm" />
      </div>
      <div v-if="snapshot.runtimes.length > 1">
        <label for="spaces-first-runtime" class="block text-sm font-medium text-silver">{{ $t('spaces.runtime') }}</label>
        <select id="spaces-first-runtime" v-model="runtimeId" :disabled="busy || !connected" class="settings-input mt-2 text-sm">
          <option v-for="runtime in snapshot.runtimes" :key="runtime.id" :value="runtime.id">{{ runtimeLabel(runtime) }}</option>
        </select>
      </div>
      <p v-else class="text-sm text-storm">{{ runtimeLabel(snapshot.runtimes[0]) }}</p>
      <p class="text-xs text-storm">{{ $t('spaces.download_note') }}</p>
      <p v-if="!hostReady" class="text-sm text-storm">{{ $t('spaces.host_first') }}</p>
      <Button type="submit" variant="outline" size="sm" :disabled="busy || !connected || !hostReady || !name.trim()">{{ $t('spaces.download') }}</Button>
    </form>
    <Button variant="ghost" size="sm" class="mt-3 text-ice" :loading="busy" :disabled="busy" data-setup-reconnect @click="refresh">
      {{ busy ? $t('spaces.checking_setup') : $t('spaces.reconnect') }}
    </Button>
    <ConfirmActionDialog v-model="restartOpen" :title="$t('spaces.restart_title')" :message="$t('spaces.restart_message')"
                         :impact-items="[$t('spaces.restart_impact_streams'), $t('spaces.restart_impact_return')]"
                         :confirm-label="$t('spaces.restart')" :cancel-label="$t('spaces.cancel')" :pending-label="$t('spaces.restart_pending')"
                         :pending="restarting" :error="restartError" :eyebrow="$t('spaces.kicker')" :impact-label="$t('spaces.dialog_impact')"
                         @confirm="confirmRestart" />
  </div>
</template>

<script setup>
import { computed, inject, onMounted, onUnmounted, ref } from 'vue'
import Button from './Button.vue'
import ConfirmActionDialog from './ConfirmActionDialog.vue'
import StatusBadge from './StatusBadge.vue'
import { requestForJob, validJobSnapshot, validSetupStart } from '../spaces-job.js'
import { docsUrl } from '../spaces-setup.js'
import { requestHostRestart } from '../restart-host.js'

defineProps({ hostReady: { type: Boolean, default: false } })
// Host Setup reads the runtime state from here, so a build without a runtime is
// not shown as a check the person has to fix.
const emit = defineEmits(['runtime'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const snapshot = ref(null), busy = ref(false), connected = ref(false), error = ref(''), notice = ref('')
const name = ref(''), runtimeId = ref(''), gpuId = ref(''), pending = ref(null)
const restarting = ref(false), restartOpen = ref(false), restartError = ref(''), restartRequested = ref(false)
const storageKey = 'polaris.spaces.first-setup'
try {
  const saved = JSON.parse(sessionStorage.getItem(storageKey) || 'null')
  if (validSetupStart(saved)) pending.value = saved
} catch { /* The host remains the authority if browser storage is unavailable. */ }
let request, poll, disposed = false, failures = 0
const runtimeLabel = runtime => runtime?.variant === 'nvidia'
  ? t('spaces.runtime_nvidia', { driver: runtime.nvidia_driver }) : t('spaces.runtime_default')

const unavailableReasons = ['already_configured', 'runtime_not_published', 'journal_fault', 'journal_locked', 'closing']
const unavailableCopy = computed(() => {
  const reason = snapshot.value?.unavailable_reason
  return unavailableReasons.includes(reason) ? t('spaces.unavailable_' + reason) : (snapshot.value?.message || t('spaces.unavailable_title'))
})
// Without a published runtime the create steps cannot run, so they are not described.
const runtimeNotPublished = computed(() => snapshot.value?.available === false && snapshot.value?.unavailable_reason === 'runtime_not_published')
const unavailableAnchor = computed(() => ({ runtime_not_published: '#preview-limits', journal_fault: '#recover-an-interrupted-setup' })[snapshot.value?.unavailable_reason] || '')
const unavailableLinkLabel = computed(() => t(snapshot.value?.unavailable_reason === 'runtime_not_published' ? 'spaces.preview_limits_link' : 'spaces.guide_section'))
const blockedReasons = ['journal_fault', 'runtime_withdrawn', 'no_eligible_gpu', 'closing']
const blockedBy = computed(() => (snapshot.value?.job?.blocked_by || []).filter(reason => blockedReasons.includes(reason)))
const blockedCopy = reason => t('spaces.blocked_' + reason)
const jobStates = ['downloading', 'preparing', 'prepared', 'cancelled', 'interrupted', 'failed', 'recovery_required', 'configuring', 'restart_required', 'activation_failed']
const jobLabel = computed(() => jobStates.includes(snapshot.value?.job?.state) ? t('spaces.job_' + snapshot.value.job.state) : '')
const jobTone = computed(() => {
  const state = snapshot.value?.job?.state
  if (state === 'prepared' || state === 'restart_required') return 'pass'
  if (state === 'downloading' || state === 'preparing' || state === 'configuring') return 'warning'
  return 'fail'
})

function adopt(next) {
  snapshot.value = next; connected.value = true
  emit('runtime', { available: next.available, reason: next.unavailable_reason || '' })
  if (next.job?.gpu_id) gpuId.value = next.job.gpu_id
  else if (!next.graphics?.some(g => g.id === gpuId.value)) gpuId.value = next.graphics?.[0]?.id || ''
  if (!next.runtimes.some(runtime => runtime.id === runtimeId.value)) runtimeId.value = next.runtimes[0]?.id || ''
  if (next.job) {
    pending.value = null
    try { sessionStorage.removeItem(storageKey) } catch { /* Server job is durable. */ }
  }
}
function working() {
  return ['downloading', 'preparing', 'configuring'].includes(snapshot.value?.job?.state)
}
// The job is polled only while it is doing something, only while the tab is
// visible, and less often while the host is not answering.
function schedule() {
  clearTimeout(poll); poll = null
  if (disposed || !working() || (typeof document !== 'undefined' && document.hidden)) return
  poll = setTimeout(refresh, Math.min(30000, 2000 * (2 ** failures)))
}
function handleVisibility() {
  if (document.hidden) { clearTimeout(poll); poll = null }
  else if (working() && !busy.value) refresh()
}
async function exchange(action) {
  if (busy.value || disposed) return
  clearTimeout(poll); poll = null
  busy.value = true; connected.value = false; error.value = ''
  request = new AbortController()
  const timeout = setTimeout(() => request.abort(), 12000)
  try {
    const response = await fetch('./api/spaces/setup/job', {
      credentials: 'include', cache: 'no-store', signal: request.signal,
      ...(action ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(action) } : {}),
    })
    if (disposed) return
    if (![200, 202, 409, 503].includes(response.status)) throw new Error(t(response.status === 404 ? 'spaces.job_404' : 'spaces.job_failed'))
    const next = await response.json()
    if (disposed) return
    if (!validJobSnapshot(next)) throw new Error(t('spaces.job_unverified'))
    adopt(next)
    failures = 0
    if (action && (!response.ok || next.accepted !== true)) error.value = t('spaces.job_rejected')
  } catch (cause) {
    failures += 1
    if (!disposed) error.value = cause.name === 'AbortError' ? t('spaces.job_timeout') : cause.message || t('spaces.job_error')
  } finally {
    clearTimeout(timeout); busy.value = false
    schedule()
  }
}
async function confirmRestart() {
  if (restarting.value || busy.value || !connected.value || restartRequested.value || snapshot.value?.job?.state !== 'restart_required') return
  restarting.value = true; restartError.value = ''; error.value = ''; notice.value = ''
  try {
    await requestHostRestart({
      onReady: () => { notice.value = t('spaces.restart_ready') },
      onTimeout: () => { notice.value = t('spaces.restart_timeout') },
    })
    restartRequested.value = true
    restartOpen.value = false
  } catch {
    connected.value = false
    restartError.value = t('spaces.restart_failed')
  } finally { restarting.value = false }
}
const refresh = () => exchange()
const send = action => exchange(action)
function start() {
  let action
  try { action = { operation: 'start', request_id: crypto.randomUUID(), runtime_id: runtimeId.value, name: name.value.trim() } }
  catch { error.value = t('spaces.secure_needed'); return }
  if (!validSetupStart(action)) { error.value = t('spaces.name_rules'); return }
  pending.value = action
  try { sessionStorage.setItem(storageKey, JSON.stringify(action)) } catch { /* Retain in memory until the host confirms. */ }
  send(action)
}
onMounted(() => {
  refresh()
  if (typeof document !== 'undefined') document.addEventListener('visibilitychange', handleVisibility)
})
onUnmounted(() => {
  disposed = true; clearTimeout(poll); request?.abort()
  if (typeof document !== 'undefined') document.removeEventListener('visibilitychange', handleVisibility)
})
</script>
