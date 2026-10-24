<template>
  <div v-if="supported || snapshot.job || loading || error || disconnected" class="surface-subtle p-4 mt-4" data-host-update-flow :aria-busy="String(loading || sending)">
    <h3 class="font-semibold text-silver" tabindex="-1" ref="heading">{{ view.title }}</h3>
    <p class="mt-2" role="status" aria-live="polite">{{ view.message }}</p>
    <p v-if="error" class="mt-2 text-warning" role="alert">{{ error }}</p>
    <p v-if="view.active" class="mt-2 text-sm">{{ stageLabel }}</p>
    <template v-if="reviewing && plan?.route === 'helper' && !view.active">
      <p class="mt-3">{{ plan.channel === 'beta' ? 'Beta' : 'Stable' }} · Fedora 44 x86_64 · Polaris {{ plan.version }}</p>
      <ul class="mt-2">
        <li v-for="asset in plan.assets" :key="asset.id" class="mt-2 break-all">
          {{ asset.package }} · {{ asset.name }}<br><span class="text-sm text-storm">Asset {{ asset.id }} · {{ asset.digest }}</span>
        </li>
      </ul>
      <p class="mt-3 text-sm">Checked against the official release’s published SHA-256 over HTTPS. This is integrity checking, not an independent publisher signature. These release assets are not a signed repository.</p>
      <p class="mt-2 text-sm">This changes package files only. Your account, paired devices, settings and library stay in their existing locations. Package scripts and triggers do not run; Polaris does not restart during installation.</p>
      <p class="mt-2 font-medium">Updates wait until streams and paused games end.</p>
      <p class="mt-2 text-sm">Administrator approval opens on this PC’s desktop. No password is entered in this page.</p>
    </template>
    <p v-if="view.busyReason" class="mt-3 text-warning" data-update-busy>{{ view.busyReason }}</p>
    <div v-if="snapshot.job || view.needsFinish" class="mt-3" data-update-installed>
      <p>Running Polaris: {{ snapshot.running_version || 'unknown' }}</p>
      <p v-for="entry in view.installed" :key="entry.name">{{ entry.name }}: {{ entry.state === 'installed' ? entry.version || 'unknown' : entry.state }}</p>
      <p v-if="snapshot.job?.detail" class="mt-2 text-sm break-all">{{ snapshot.job.detail }}</p>
      <p v-if="snapshot.job?.check?.code" class="mt-2 text-sm">{{ snapshot.job.check.code }}</p>
    </div>
    <div class="flex flex-wrap gap-3 mt-4">
      <button v-if="view.canReview && !view.finished" class="focus-ring system-button system-button-secondary" :disabled="loading || sending" @click="reviewUpdate">
        {{ snapshot.job ? 'Review again' : 'Review update' }}
      </button>
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
import { describeHostUpdate } from '../update-center.js'

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
const now = ref(Date.now())
const supported = computed(() => plan.value?.route === 'helper')
const view = computed(() => describeHostUpdate(snapshot.value, plan.value, {
  reviewing: reviewing.value, disconnected: disconnected.value, restarting: Boolean(restarting.value),
  restartTimedOut: Boolean(restarting.value && now.value - restarting.value > 90000),
}))
watch(view, (value) => emit('state', value), { immediate: true })
const stageLabel = computed(() => ({ checking_release: 'Checking the official release again…', downloading: 'Downloading official release assets…',
  verifying: 'Verifying package identities and SHA-256…', checking_transaction: 'Checking dependencies and the bounded RPM transaction…',
  installing: 'Installing RPM packages…', waiting_for_approval: 'Waiting for approval at this PC’s desktop…',
  restarting: 'Waiting for this PC to respond…' })[view.value.stage] || 'Reading this PC’s progress…')
let timer = null
let disposed = false
let generation = 0

async function readJson(url) {
  const response = await fetch(url, { credentials: 'include', cache: 'no-store' })
  if (!response.ok) throw new Error('Host update information is unavailable')
  return response.json()
}

async function poll() {
  try {
    const value = await readJson('./api/update/job')
    if (disposed) return
    snapshot.value = value
    if (disconnected.value) error.value = ''
    disconnected.value = false
    if (view.value.finished) restarting.value = 0
  } catch {
    if (!disposed && (supported.value || snapshot.value.job || restarting.value)) disconnected.value = true
  } finally {
    now.value = Date.now()
    if (!disposed) timer = setTimeout(poll, 3000)
  }
}

async function loadPlan(showReview = false) {
  const current = ++generation
  reviewing.value = false
  loading.value = true
  error.value = ''
  try {
    const value = await readJson(`./api/update/plan?channel=${props.channel}`)
    if (disposed || current !== generation) return
    plan.value = value
    emit('plan', value)
    reviewing.value = showReview && value.route === 'helper'
    if (showReview) {
      await nextTick()
      heading.value?.focus()
    }
  } catch {
    if (!disposed && current === generation) error.value = 'The host could not check the official release. Check again before reviewing.'
  } finally {
    if (current === generation) loading.value = false
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
  error.value = ''
  try {
    const response = await fetch(url, { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': document.querySelector('meta[name="csrf-token"]')?.content || '' },
      body: JSON.stringify(body) })
    const value = await response.json()
    if (disposed) return
    snapshot.value = value
    if (!response.ok || value.accepted !== true) {
      error.value = value.refusal?.message || value.error || 'The host refused this action. Read its state, then review again.'
      restarting.value = 0
    }
  } catch {
    // Never replay an ambiguous mutation. Poll the existing host job instead.
    disconnected.value = true
    error.value = 'Connection lost. Reading the host job before any further action.'
  } finally {
    sending.value = false
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
  restarting.value = Date.now()
  await mutate('./api/update/finish', { action: 'finish', request_id: crypto.randomUUID(),
    version: view.value.installed.find((entry) => entry.name === 'polaris').version })
}

watch(() => props.channel, () => { plan.value = null; emit('plan', null); loadPlan() })
onMounted(() => { loadPlan(); poll() })
onUnmounted(() => { disposed = true; generation++; clearTimeout(timer) })
defineExpose({ reviewUpdate, refresh: () => loadPlan() })
</script>
