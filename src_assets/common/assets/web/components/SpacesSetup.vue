<template>
  <details class="section-card settings-disclosure" :open="open" :aria-busy="loading" data-spaces-setup @toggle="open = $event.target.open">
    <summary class="settings-disclosure-summary focus-ring cursor-pointer rounded py-2 font-semibold text-silver">
      <span class="flex flex-wrap items-center gap-2">
        <span>{{ $t('spaces.host_setup') }}</span>
        <StatusBadge :status="summaryTone" :label="summaryLabel" />
      </span>
      <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7" />
      </svg>
    </summary>
    <div class="mt-2 flex flex-wrap items-start justify-between gap-3">
      <div>
        <p class="section-kicker">{{ $t('spaces.kicker') }}</p>
        <h2 id="spaces-setup-title" class="section-title">{{ $t('spaces.check_host') }}</h2>
        <p v-if="!setup?.available" class="mt-2 max-w-2xl text-sm text-storm">{{ $t('spaces.host_copy') }}</p>
      </div>
      <Button variant="outline" size="sm" :loading="loading" :disabled="loading" data-spaces-recheck @click="refresh">
        {{ loading ? $t('spaces.checking') : $t('spaces.recheck') }}
      </Button>
    </div>
    <p v-if="error" class="mt-4 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <p v-else-if="loading" class="mt-4 text-sm text-storm" role="status">{{ $t('spaces.checking_copy') }}</p>
    <template v-if="setup">
      <p class="mt-4 text-sm text-silver" role="status" aria-live="polite">
        {{ setup.host_prerequisites_ready ? $t('spaces.host_ready') : $t('spaces.host_steps') }}
        {{ setup.available ? $t('spaces.host_available') : runtimeWaiting ? $t('spaces.host_waiting_runtime') : $t('spaces.host_not_available') }}
      </p>
      <details class="settings-disclosure mt-4" :open="checksOpen" @toggle="checksOpen = $event.target.open">
        <summary class="settings-disclosure-summary focus-ring cursor-pointer rounded py-2 text-sm text-ice">
          <span>{{ $t('spaces.setup_checks') }}</span>
          <span class="flex items-center gap-2">
            <span class="control-chip" data-setup-count>{{ readyCount }}/{{ countedChecks.length }}</span>
            <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
              <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7" />
            </svg>
          </span>
        </summary>
        <ol class="mt-3 grid gap-3 md:grid-cols-2">
          <li v-for="check in visibleChecks" :key="check.id" class="min-w-0 rounded-xl border bg-deep/40 p-4"
              :class="waitingCheck(check) ? 'border-storm/20' : statusTone(checkStatus(check)).card" :data-setup-check="check.id">
            <div class="flex flex-wrap items-center justify-between gap-2">
              <h3 class="text-sm font-semibold text-silver">{{ check.title }}</h3>
              <span v-if="waitingCheck(check)" class="meta-pill border border-storm/30 bg-deep/40 text-storm" data-check-waiting>
                {{ $t('spaces.check_waiting_runtime') }}
              </span>
              <StatusBadge v-else :status="checkStatus(check)" :label="checkLabel(check)" />
            </div>
            <p class="mt-2 text-sm text-storm">{{ waitingCheck(check) && check.id === 'spaces' ? $t('spaces.check_waiting_runtime_detail') : check.detail }}</p>
            <div v-if="showsHostAction(check)" class="mt-3 space-y-2" data-host-action>
              <p v-if="hostJobFor(check) && hostWorking" class="text-sm text-silver" role="status" aria-live="polite" data-host-action-state>
                {{ hostJobCopy(hostJobFor(check)) }}
              </p>
              <p v-else-if="hostJobFor(check)" class="text-sm text-silver" role="status" data-host-action-outcome>{{ hostJobCopy(hostJobFor(check)) }}</p>
              <p v-if="hostJobFor(check) && hostJobDetail(hostJobFor(check))" class="text-xs text-storm" data-host-action-detail>
                {{ hostJobDetail(hostJobFor(check)) }}
              </p>
              <p v-if="hostError && hostErrorFor === check.host_action" class="text-sm text-warning-bright" role="alert">{{ hostError }}</p>
              <template v-if="offersHostAction(check) && !(hostWorking && hostJobFor(check))">
                <div class="flex flex-wrap items-center gap-3">
                  <Button variant="outline" size="sm" :loading="hostSending === check.host_action" :disabled="hostBlocked"
                          :aria-label="hostActionLabel(check) + ': ' + check.title" data-host-action-start @click="startHostAction(check)">
                    {{ hostActionLabel(check) }}
                  </Button>
                </div>
                <p v-if="hostUnavailable" class="text-xs text-warning-bright" data-host-action-unavailable>{{ hostUnavailable }}</p>
                <p v-else class="text-xs text-storm">{{ hostPromptWhere }}</p>
              </template>
            </div>
            <div v-if="check.id === 'runtime' && runtimeActive(check)" class="mt-3 space-y-2" data-runtime-action>
              <p v-if="runtimeDownloading" class="text-sm text-silver" role="status" aria-live="polite">{{ $t('spaces.runtime_downloading') }}</p>
              <p v-else-if="downloadOutcome" class="text-sm text-silver" role="status" data-runtime-outcome>{{ downloadOutcome }}</p>
              <p v-if="runtimeError" class="text-sm text-warning-bright" role="alert">{{ runtimeError }}</p>
              <div v-if="canDownload(check) || runtime?.download?.can_cancel" class="flex flex-wrap items-center gap-3">
                <Button v-if="canDownload(check)" variant="outline" size="sm" :loading="runtimeSending" :disabled="downloadBlocked"
                        :aria-label="downloadLabel(check) + ': ' + check.title" data-runtime-download @click="startDownload(check)">
                  {{ downloadLabel(check) }}
                </Button>
                <Button v-if="runtime?.download?.can_cancel" variant="ghost" size="sm" class="text-warning-bright"
                        :disabled="runtimeSending" data-runtime-stop @click="stopDownload">
                  {{ $t('spaces.runtime_stop') }}
                </Button>
              </div>
              <p v-if="canDownload(check) && !setup.host_prerequisites_ready" class="text-xs text-storm">{{ $t('spaces.runtime_host_first') }}</p>
            </div>
            <div v-if="check.state !== 'ready' && !waitingCheck(check)" class="mt-3 flex flex-wrap gap-3">
              <a :href="guideHref(check)" target="_blank" rel="noopener noreferrer" class="focus-ring inline-block rounded py-2 text-sm text-ice hover:underline">
                {{ guideLabel(check) }}
              </a>
              <router-link v-if="['input', 'gpu', 'security'].includes(check.id)" to="/troubleshooting"
                           class="focus-ring inline-block rounded py-2 text-sm text-ice hover:underline">
                {{ $t('spaces.open_doctor') }}
              </router-link>
            </div>
            <div v-if="stepsFor(check).length" class="mt-3 space-y-2" data-setup-steps>
              <p class="text-xs text-storm">{{ $t('spaces.steps_copy') }}</p>
              <div v-for="step in stepsFor(check)" :key="step.command" class="rounded-lg border border-storm/20 bg-void/40 p-2">
                <div class="flex flex-wrap items-start justify-between gap-2">
                  <span class="text-xs text-silver">{{ step.title }}</span>
                  <Button variant="ghost" size="sm" class="text-ice" :aria-label="$t('spaces.copy') + ': ' + step.title" @click="copy(step.command)">
                    {{ copied === step.command ? $t('spaces.copied') : $t('spaces.copy') }}
                  </Button>
                </div>
                <code class="mt-1 block whitespace-pre-wrap break-all font-mono text-xs text-silver">{{ step.command }}</code>
              </div>
            </div>
            <p v-if="check.id === 'spaces' && !setup.configured && !waitingCheck(check)" class="mt-3 text-sm text-storm">
              {{ $t('spaces.spaces_hint') }}<template v-if="!runtimeCheck"> {{ $t('spaces.spaces_hint_runtime') }}</template>
            </p>
          </li>
        </ol>
      </details>
      <p v-if="!setup.available" class="mt-4 text-xs text-storm">{{ $t('spaces.no_mutation') }}</p>
      <SpacesFirstSetup v-if="!setup.configured" id="spaces-prepare" ref="firstSetup" :host-ready="setup.host_prerequisites_ready" @runtime="runtime = $event" />
    </template>
  </details>
</template>

<script setup>
import { computed, inject, onMounted, onUnmounted, ref, watch } from 'vue'
import Button from './Button.vue'
import StatusBadge from './StatusBadge.vue'
import SpacesFirstSetup from './SpacesFirstSetup.vue'
import { statusTone } from '../status-tones.js'
import { guideHref, setupSteps, validSetup } from '../spaces-setup.js'
import { hostActions, hostActionWorking, validHostActionSnapshot } from '../spaces-job.js'

const emit = defineEmits(['state', 'runtime-waiting'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const setup = ref(null), loading = ref(false), error = ref(''), copied = ref('')
// Both disclosures open themselves when something needs attention and never
// close under the reader: a successful recheck leaves the page where it was.
const open = ref(true), checksOpen = ref(true)
let request, seen = false, copyTimer
onUnmounted(() => { request?.abort(); clearTimeout(copyTimer) })

// What the first-Space setup last heard from the host about its jobs, including
// a download started from the gaming runtime check.
const runtime = ref(null)
const firstSetup = ref(null)
const runtimeCheck = computed(() => setup.value?.checks.find(check => check.id === 'runtime') || null)
// A build with no verified gaming runtime cannot create a Space, and nothing on
// this PC can change that. Its runtime check then waits instead of asking for
// attention, and the count covers only the checks a person can act on. Hosts
// from before that check report the same through first-Space setup.
const runtimeWaiting = computed(() => !!setup.value && !setup.value.configured && (runtimeCheck.value
  ? runtimeCheck.value.runtime.status === 'not_published'
  : runtime.value?.available === false && runtime.value?.reason === 'runtime_not_published'))
watch(runtimeWaiting, waiting => emit('runtime-waiting', waiting))
const waitingCheck = check => check.id === 'runtime' ? check.runtime?.status === 'not_published'
  : !runtimeCheck.value && runtimeWaiting.value && check.id === 'spaces' && check.state === 'not_configured'
// One waiting row: the runtime check says why no Space can be configured yet.
const visibleChecks = computed(() => (setup.value?.checks || []).filter(check =>
  !(runtimeCheck.value && runtimeWaiting.value && check.id === 'spaces' && check.state === 'not_configured')))
const countedChecks = computed(() => visibleChecks.value.filter(check => !waitingCheck(check)))
const readyCount = computed(() => countedChecks.value.filter(check => check.state === 'ready').length)

// The runtime download runs as a host job, through first-Space setup's
// connection. The check turns ready once a recheck finds the verified image.
const runtimeSending = ref(false), runtimeError = ref('')
const runtimeDownloading = computed(() => runtime.value?.download?.state === 'downloading' || runtime.value?.job === 'downloading')
const runtimeNamed = check => ['available', 'ready', 'failed'].includes(check.runtime?.status)
const canDownload = check => ['available', 'failed'].includes(check.runtime?.status) && !setup.value.configured &&
  runtime.value?.available === true && !runtimeDownloading.value
const downloadBlocked = computed(() => runtimeSending.value || loading.value || !setup.value?.host_prerequisites_ready)
const downloadLabel = check => t(check.runtime.status === 'failed' ? 'spaces.runtime_retry' : 'spaces.runtime_download')
const downloadOutcome = computed(() => ['failed', 'cancelled'].includes(runtime.value?.download?.state) &&
  runtimeCheck.value?.runtime.status !== 'ready' ? runtime.value.download.message : '')
// Progress, an outcome or a button to show; a verified runtime needs none.
const runtimeActive = check => ['available', 'failed'].includes(check.runtime?.status) && (runtimeDownloading.value ||
  !!downloadOutcome.value || !!runtimeError.value || canDownload(check) || !!runtime.value?.download?.can_cancel)
async function startDownload(check) {
  if (downloadBlocked.value || !firstSetup.value) return
  runtimeSending.value = true; runtimeError.value = ''
  try { runtimeError.value = await firstSetup.value.download(check.runtime.id) || '' }
  finally { runtimeSending.value = false }
}
async function stopDownload() {
  if (runtimeSending.value || !firstSetup.value) return
  runtimeSending.value = true; runtimeError.value = ''
  try { runtimeError.value = await firstSetup.value.stopDownload() || '' }
  finally { runtimeSending.value = false }
}
// A download that ends, from here or from first-Space setup, changes what the
// runtime check finds.
let recheckAfterLoad = false
watch(runtimeDownloading, (downloading, was) => {
  if (!was || downloading) return
  if (loading.value) recheckAfterLoad = true
  else refresh()
})
const hostReadyAndWaiting = computed(() => runtimeWaiting.value && setup.value?.host_prerequisites_ready)
const summaryTone = computed(() => error.value ? 'fail' : setup.value?.available && setup.value?.host_prerequisites_ready ? 'pass' : setup.value?.configured ? 'fail' : hostReadyAndWaiting.value ? 'pass' : 'warning')
const summaryLabel = computed(() => error.value ? t('spaces.host_attention') :
  setup.value?.available && setup.value?.host_prerequisites_ready ? t('spaces.host_configured') :
  setup.value?.configured ? t('spaces.host_attention') : hostReadyAndWaiting.value ? t('spaces.host_ready_badge') : t('spaces.host_set_up'))
// A runtime that is only waiting to be downloaded is a step, not a fault.
const runtimeStep = check => check.id === 'runtime' && (check.runtime?.status === 'available' ||
  (runtimeDownloading.value && runtimeNamed(check) && check.runtime.status !== 'ready'))
const checkStatus = check => runtimeStep(check) ? 'warning' :
  check.state === 'ready' ? 'pass' : check.state === 'not_configured' ? 'warning' : 'fail'
const checkLabel = check => runtimeStep(check)
  ? t(runtimeDownloading.value ? 'spaces.job_downloading' : 'spaces.runtime_not_downloaded')
  : t(check.state === 'ready' ? 'spaces.check_ready' : check.state === 'not_configured' ? 'spaces.check_not_configured' : 'spaces.check_required')
const guideLabel = check => t(check.id === 'security' ? 'spaces.guide_security' : ['docker', 'docker_access'].includes(check.id) ? 'spaces.guide_docker' : 'spaces.guide_section')
const stepsFor = check => setupSteps(setup.value, check)

// Host setup an administrator approves at the PC that runs Polaris. The host runs one change at a
// time; this page asks about it only while it waits for approval or runs, and only in a visible tab.
const hostChecks = { security_install: 'security', docker_access: 'docker_access' }
const hostReasons = ['closing', 'helper_missing', 'host_setup_running', 'host_unknown', 'image_based_host', 'no_local_desktop',
  'pkexec_missing', 'policy_missing', 'setup_active', 'spaces_active', 'stream_active']
const hostAction = ref(null), hostSending = ref(''), hostError = ref(''), hostErrorFor = ref('')
// Jobs this page started or saw in progress. An old outcome from before the page opened stays quiet.
const hostSeen = ref([])
let hostPoll, hostRequest, hostFailures = 0, hostDisposed = false
const hostWorking = computed(() => hostActionWorking.includes(hostAction.value?.job?.state))
const offersHostAction = check => !!hostAction.value && check.state !== 'ready' && hostActions.includes(check.host_action)
const hostJobFor = check => {
  const job = hostAction.value?.job
  return job && hostChecks[job.action] === check.id && hostSeen.value.includes(job.request_id) ? job : null
}
const showsHostAction = check => offersHostAction(check) || !!hostJobFor(check)
const hostActionLabel = check => t('spaces.host_action_' + check.host_action)
const hostJobCopy = job => t(job.state === 'done' ? 'spaces.host_action_done_' + job.action : 'spaces.host_action_' + job.state)
// The helper's own sentence says what stopped it; after a success the recheck speaks instead.
const hostJobDetail = job => ['refused', 'failed'].includes(job.state) ? job.detail : ''
const hostReason = (code, message) => hostReasons.includes(code) ? t('spaces.host_action_unavailable_' + code) : message
const hostUnavailable = computed(() => hostAction.value && !hostAction.value.available ?
  hostReason(hostAction.value.reason, hostAction.value.message) : '')
const hostBlocked = computed(() => !!hostSending.value || hostWorking.value || loading.value || !hostAction.value?.available)
// A browser on this PC shows the prompt on its own screen; anywhere else, the prompt is not where the reader is.
const promptLocal = typeof window !== 'undefined' && ['localhost', '127.0.0.1', '[::1]', '::1'].includes(window.location.hostname)
const hostPromptWhere = computed(() => t(promptLocal ? 'spaces.host_action_prompt_local' : 'spaces.host_action_prompt_remote'))

function adoptHostAction(next) {
  const was = hostAction.value?.job
  hostAction.value = next
  const job = next.job
  if (job && hostActionWorking.includes(job.state) && !hostSeen.value.includes(job.request_id)) hostSeen.value = [...hostSeen.value, job.request_id]
  // A change that just finished is checked again at once.
  if (was && hostActionWorking.includes(was.state) && job?.request_id === was.request_id && !hostActionWorking.includes(job.state)) {
    if (loading.value) recheckAfterLoad = true
    else refresh()
  }
}
function scheduleHostAction() {
  clearTimeout(hostPoll); hostPoll = null
  if (hostDisposed || !hostWorking.value || (typeof document !== 'undefined' && document.hidden)) return
  hostPoll = setTimeout(() => { hostExchange().catch(() => {}) }, Math.min(30000, 2000 * (2 ** hostFailures)))
}
async function hostExchange(body) {
  clearTimeout(hostPoll); hostPoll = null
  if (hostDisposed) return null
  hostRequest?.abort()
  const controller = new AbortController()
  hostRequest = controller
  const timeout = setTimeout(() => controller.abort(), 12000)
  try {
    const response = await fetch('./api/spaces/setup/host-action', {
      credentials: 'include', cache: 'no-store', signal: controller.signal,
      ...(body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) } : {}),
    })
    if (hostDisposed) return null
    // A host without in-app setup offers no button; its terminal steps stay.
    if (response.status === 404) { hostAction.value = null; return null }
    if (![200, 202, 409, 503].includes(response.status)) throw new Error(t('spaces.host_action_rejected'))
    const next = await response.json()
    if (hostDisposed) return null
    if (!validHostActionSnapshot(next)) throw new Error(t('spaces.host_action_unreachable'))
    hostFailures = 0
    adoptHostAction(next)
    return { status: response.status, snapshot: next }
  } catch (cause) {
    hostFailures += 1
    throw cause
  } finally {
    clearTimeout(timeout)
    scheduleHostAction()
  }
}
async function startHostAction(check) {
  if (hostBlocked.value || !offersHostAction(check)) return
  let requestId
  try { requestId = crypto.randomUUID() } catch { hostErrorFor.value = check.host_action; hostError.value = t('spaces.secure_needed'); return }
  hostSending.value = check.host_action; hostErrorFor.value = check.host_action; hostError.value = ''
  hostSeen.value = [...hostSeen.value, requestId]
  try {
    const result = await hostExchange({ action: check.host_action, request_id: requestId })
    if (result && (result.status === 409 || result.status === 503 || result.snapshot.accepted === false))
      hostError.value = hostReason(result.snapshot.refusal?.code, result.snapshot.refusal?.message || t('spaces.host_action_rejected'))
  } catch (cause) {
    hostError.value = cause.name === 'AbortError' ? t('spaces.host_action_unreachable') : cause.message || t('spaces.host_action_unreachable')
  } finally { hostSending.value = '' }
}
function handleHostVisibility() {
  if (document.hidden) { clearTimeout(hostPoll); hostPoll = null }
  else if (hostWorking.value) hostExchange().catch(() => {})
}
if (typeof document !== 'undefined') document.addEventListener('visibilitychange', handleHostVisibility)
onUnmounted(() => {
  hostDisposed = true; clearTimeout(hostPoll); hostRequest?.abort()
  if (typeof document !== 'undefined') document.removeEventListener('visibilitychange', handleHostVisibility)
})

function settle(next) {
  const needsAttention = !next.available || !next.host_prerequisites_ready
  const runtimeStatus = next.checks.find(check => check.id === 'runtime')?.runtime.status
  const checksNeedAttention = !next.host_prerequisites_ready || (next.configured && !next.available) ||
    (!next.configured && ['unsupported', 'available', 'failed'].includes(runtimeStatus))
  if (!seen) { open.value = needsAttention; checksOpen.value = checksNeedAttention; seen = true }
  else { if (needsAttention) open.value = true; if (checksNeedAttention) checksOpen.value = true }
  emit('state', { available: next.available, configured: next.configured, hostReady: next.host_prerequisites_ready })
}

async function copy(command) {
  try {
    await navigator.clipboard.writeText(command)
    copied.value = command
    clearTimeout(copyTimer)
    copyTimer = setTimeout(() => { copied.value = '' }, 2000)
  } catch { /* The command stays readable on the card. */ }
}

async function refresh() {
  if (loading.value) return
  loading.value = true; error.value = ''
  request = new AbortController()
  const timeout = setTimeout(() => request.abort(), 12000)
  try {
    const response = await fetch('./api/spaces/setup', { credentials: 'include', cache: 'no-store', signal: request.signal })
    if (!response.ok) throw new Error(t(response.status === 404 ? 'spaces.setup_404' : 'spaces.setup_failed'))
    const next = await response.json()
    if (!validSetup(next)) throw new Error(t('spaces.setup_unverified'))
    setup.value = next
    settle(next)
    // Whether an administrator can fix a check from here is asked of the host, not assumed.
    if (next.checks.some(check => check.state !== 'ready' && hostActions.includes(check.host_action)) || hostWorking.value)
      hostExchange().catch(() => {})
  } catch (cause) {
    setup.value = null
    open.value = true
    error.value = cause.name === 'AbortError' ? t('spaces.setup_timeout') : cause.message || t('spaces.setup_failed')
    emit('state', null)
  } finally {
    clearTimeout(timeout); loading.value = false
    if (recheckAfterLoad) { recheckAfterLoad = false; refresh() }
  }
}
onMounted(refresh)
</script>
