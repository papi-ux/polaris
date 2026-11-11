<template>
  <div v-if="supported || snapshot.job || view.needsFinish || view.needsAttention || loading || error || disconnected" class="surface-subtle p-4 mt-4" data-host-update-flow :aria-busy="String(loading || sending)">
    <h3 ref="heading" class="font-semibold text-silver" tabindex="-1">{{ view.title }}</h3>
    <div role="status" aria-live="polite" aria-atomic="true" data-update-announcement>
      <p class="mt-2">{{ view.message }}</p>
      <p v-if="view.active" class="mt-2 text-sm">{{ stageLabel }}</p>
      <p v-if="view.busyReason" class="mt-3 text-warning" data-update-busy>{{ view.busyReason }}</p>
    </div>
    <p v-if="error" class="mt-2 text-warning" role="alert">{{ error }}</p>
    <template v-if="reviewing && plan?.route === 'helper' && !view.active">
      <p class="mt-3">{{ selectedReleaseLabel(plan) }} · Fedora {{ plan.host?.version_id || '' }} x86_64 · Polaris {{ plan.version }}</p>
      <ul class="mt-2">
        <li v-for="asset in plan.assets" :key="asset.id" class="mt-2">
          {{ asset.package === 'polaris' ? 'Polaris' : 'DRM/KMS capture helper' }} · {{ plan.version }}
          <span v-if="plan.packages_to_update && !plan.packages_to_update.includes(asset.package)"> · already installed; kept</span>
        </li>
      </ul>
      <p class="mt-3 text-sm">Downloads are checked against the official release’s published SHA-256 over HTTPS. Before installing, Polaris also verifies its signature on each package. An unsigned package or a different signing key stops the update.</p>
      <p class="mt-2 text-sm">Your account, paired devices, settings and library stay in their existing locations. Polaris keeps running until you choose Finish.</p>
      <p class="mt-2 text-sm">This update keeps the existing streaming setup. Package setup scripts and cache triggers are skipped, so device rules or desktop icons may not refresh until your next sign-in or reboot. If streaming setup needs attention, use the guided setup steps after Finish.</p>
      <p class="mt-2 font-medium">Updates wait until streams and paused games end.</p>
      <p v-if="plan.update_needed" class="mt-2 text-sm">Administrator approval opens on this PC’s desktop. If needed, it also adds the packaged Polaris signing key to this PC’s trusted keys. No password is entered in this page.</p>
    </template>
    <div v-if="snapshot.job || view.needsFinish || view.needsAttention" class="mt-3" data-update-installed>
      <p>Running Polaris: {{ snapshot.running_version || 'unknown' }}</p>
      <p v-for="entry in view.installed" :key="entry.name">{{ entry.name }}: {{ entry.state === 'installed' ? entry.version || 'unknown' : entry.state }}<span v-if="entry.versions.length"> ({{ entry.versions.join(', ') }})</span></p>
      <p v-if="view.needsAttention" class="mt-2 text-sm">{{ view.recovery }}</p>
    </div>
    <details v-if="plan?.assets?.length || snapshot.job?.detail || snapshot.job?.check?.code" class="mt-3 text-sm" data-update-technical-details>
      <summary class="focus-ring cursor-pointer">Details</summary>
      <p v-if="plan?.release_id" class="mt-2">Official release {{ plan.release_id }}</p>
      <p v-for="asset in plan?.assets || []" :key="asset.id" class="mt-2 break-all">{{ asset.name }} · Asset {{ asset.id }}<br>{{ asset.digest }}</p>
      <p v-if="snapshot.job?.detail" class="mt-2 break-all">{{ snapshot.job.detail }}</p>
      <p v-if="snapshot.job?.check?.code" class="mt-2">Support code: {{ snapshot.job.check.code }}</p>
    </details>
    <div class="flex flex-wrap gap-3 mt-4">
      <button v-if="view.canReview && !view.finished" class="focus-ring system-button system-button-secondary" :disabled="loading || sending" @click="reviewUpdate">{{ snapshot.job ? 'Review again' : 'Review update' }}</button>
      <button v-if="reviewing && plan?.update_needed && !view.active" data-update-install class="focus-ring system-button system-button-primary" :disabled="!view.canInstall || sending" @click="installUpdate">Install with host approval</button>
      <button v-if="view.canCancel" data-update-cancel class="focus-ring system-button system-button-secondary" :disabled="sending" @click="cancelUpdate">Cancel approval</button>
      <button v-if="view.needsFinish" data-update-finish class="focus-ring system-button system-button-primary" :disabled="!view.canFinish || sending" @click="finishUpdate">Finish</button>
    </div>
    <p v-if="view.needsFinish && !view.canFinish" class="mt-2 text-sm">{{ snapshot.finish_refusal?.message || 'Finish when streams and paused games end. Recheck this PC if its installed state is unknown.' }}</p>
  </div>
  <p v-else-if="plan?.refusal?.message" class="mt-3 text-sm" data-update-guided>{{ plan.refusal.message }} The guided steps remain available below.</p>
</template>

<script setup>
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { describeHostUpdate, hostUpdatePollDelay, selectedReleaseLabel } from '../update-center.js'

const props = defineProps({ channel: { type: String, default: 'stable' } })
const emit = defineEmits(['plan', 'state'])
const plan = ref(null)
const snapshot = ref({})
const reviewing = ref(false)
const loading = ref(false)
const sending = ref(false)
const disconnected = ref(false)
const error = ref('')
const heading = ref(null)
const restarting = ref(0)
const finishTarget = ref('')
const now = ref(Date.now())
const supported = computed(() => plan.value?.route === 'helper')
const view = computed(() => describeHostUpdate(snapshot.value, plan.value, {
  reviewing: reviewing.value, disconnected: disconnected.value, restarting: Boolean(restarting.value), finishTarget: finishTarget.value,
  restartTimedOut: Boolean(restarting.value && now.value - restarting.value > 90000),
}))
watch(view, (value) => emit('state', value), { immediate: true })
const stageLabel = computed(() => ({ checking_release: 'Checking the official release again…', downloading: 'Downloading official release assets…',
  verifying: 'Verifying package identities and SHA-256…', checking_transaction: 'Checking whether these packages can be installed safely…',
  verifying_signatures: 'Verifying the Polaris package signatures…', trusting_vendor_key: 'Checking this PC’s trusted Polaris key…',
  vendor_key_imported: 'The Polaris signing key was added. Checking the package transaction…',
  installing: 'Installing RPM packages…', waiting_for_approval: 'Waiting for approval at this PC’s desktop…',
  restarting: 'Waiting for this PC to respond…' })[view.value.stage] || 'Reading this PC’s progress…')
let timer = null
let disposed = false
let generation = 0
let snapshotEpoch = 0
let polling = false
const controllers = new Set()

async function request(url, options = {}) {
  const controller = new AbortController()
  controllers.add(controller)
  const timeout = setTimeout(() => controller.abort(), url.includes('/plan?') ? 45000 : 10000)
  try {
    const response = await fetch(url, { credentials: 'include', cache: 'no-store', ...options, signal: controller.signal })
    const value = await response.json()
    return { response, value }
  } finally {
    clearTimeout(timeout)
    controllers.delete(controller)
  }
}

async function readJson(url) {
  const { response, value } = await request(url)
  if (!response.ok || !value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Host update information is unavailable')
  return value
}

function schedulePoll(immediate = false) {
  clearTimeout(timer)
  if (disposed) return
  const delay = hostUpdatePollDelay({ visible: document.visibilityState !== 'hidden', active: view.value.active,
    reviewing: reviewing.value, restarting: Boolean(restarting.value), disconnected: disconnected.value,
    restartTimedOut: Boolean(restarting.value && now.value - restarting.value > 90000),
    capable: supported.value || Boolean(snapshot.value.job) || Boolean(restarting.value) })
  if (delay !== null) timer = setTimeout(poll, immediate ? 0 : delay)
}

async function poll() {
  if (disposed || polling || document.visibilityState === 'hidden') return
  polling = true
  const epoch = snapshotEpoch
  try {
    const value = await readJson('./api/update/job')
    if (disposed || epoch !== snapshotEpoch) return
    snapshot.value = value
    if (disconnected.value) error.value = ''
    disconnected.value = false
    if (view.value.finishConfirmed || view.value.finished) restarting.value = 0
  } catch {
    if (!disposed && epoch === snapshotEpoch && (supported.value || snapshot.value.job || restarting.value)) disconnected.value = true
  } finally {
    polling = false
    now.value = Date.now()
    schedulePoll()
  }
}

async function focusHeading() {
  await nextTick()
  if (!disposed) heading.value?.focus()
}

function visibilityChanged() {
  now.value = Date.now()
  schedulePoll(true)
}

async function loadPlan(showReview = false, refresh = showReview) {
  const current = ++generation
  reviewing.value = false
  loading.value = true
  error.value = ''
  try {
    const value = await readJson(`./api/update/plan?channel=${props.channel}${refresh ? '&refresh=1' : ''}`)
    if (disposed || current !== generation) return
    plan.value = value
    emit('plan', value)
    reviewing.value = showReview && value.route === 'helper'
    if (showReview) await focusHeading()
  } catch {
    if (!disposed && current === generation) error.value = 'The host could not check the official release. Check again before reviewing.'
  } finally {
    if (current === generation) loading.value = false
    schedulePoll()
  }
}

async function reviewUpdate() {
  // Every retry obtains a fresh host review, nonce and subsequent OS approval.
  if (!view.value.canReview || sending.value) return
  snapshot.value = { ...snapshot.value, job: null }
  await loadPlan(true)
}

async function mutate(url, body) {
  if (sending.value) return
  sending.value = true
  snapshotEpoch++
  error.value = ''
  await focusHeading()
  try {
    const { response, value } = await request(url, { method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': document.querySelector('meta[name="csrf-token"]')?.content || '' },
      body: JSON.stringify(body) })
    if (disposed) return
    if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Host update response is unavailable')
    snapshotEpoch++
    snapshot.value = value
    if (!response.ok || value.accepted !== true) {
      error.value = value.refusal?.message || value.error || 'The host refused this action. Read its state, then review again.'
      restarting.value = 0
      finishTarget.value = ''
    }
  } catch {
    // Never replay an ambiguous mutation. Poll the existing host job instead.
    if (!disposed) {
      disconnected.value = true
      error.value = 'Connection lost. Reading the host job before any further action.'
    }
  } finally {
    sending.value = false
    await focusHeading()
    schedulePoll(true)
  }
}

async function installUpdate() {
  if (!view.value.canInstall) return
  const reviewed = plan.value
  reviewing.value = false
  plan.value = { ...reviewed, review_id: '' }
  await mutate('./api/update/job', { action: 'install', request_id: crypto.randomUUID(),
    channel: reviewed.channel, version: reviewed.version, review_id: reviewed.review_id })
}

async function cancelUpdate() {
  if (view.value.canCancel) await mutate('./api/update/job', { action: 'cancel', request_id: snapshot.value.job.request_id })
}

async function finishUpdate() {
  if (!view.value.canFinish) return
  finishTarget.value = view.value.installed.find((entry) => entry.name === 'polaris').version
  restarting.value = Date.now()
  now.value = Date.now()
  await mutate('./api/update/finish', { action: 'finish', request_id: crypto.randomUUID(),
    version: finishTarget.value })
}

watch(() => props.channel, () => { plan.value = null; emit('plan', null); loadPlan() })
onMounted(() => { document.addEventListener('visibilitychange', visibilityChanged); loadPlan(); poll() })
onUnmounted(() => { disposed = true; generation++; clearTimeout(timer); document.removeEventListener('visibilitychange', visibilityChanged); for (const controller of controllers) controller.abort() })
defineExpose({ reviewUpdate, refresh: () => loadPlan(false, true) })
</script>
