<template>
  <div v-if="visible" class="mt-3 rounded-xl border p-3 text-sm" :class="done ? 'border-storm/20 bg-deep/40' : 'border-warning/30 bg-warning/10'"
       data-runtime-move :data-state="jobState || (mismatch ? 'mismatch' : '')">
    <template v-if="mismatch || upgrade || updated">
      <template v-if="mismatch">
        <p class="font-semibold text-warning-bright">{{ $t('spaces.runtime_mismatch_title') }}</p>
        <p class="mt-1 text-silver" data-runtime-detail>
          {{ $t('spaces.runtime_mismatch_detail', { runtime: space.runtime_driver, host: space.host_driver }) }}
        </p>
        <p class="mt-1 text-storm">{{ $t('spaces.runtime_mismatch_blocked') }}</p>
      </template>
      <template v-else-if="upgrade">
        <p class="font-semibold text-silver">{{ $t('spaces.runtime_upgrade_title') }}</p>
        <p class="mt-1 text-silver" data-runtime-upgrade-detail>
          {{ $t('spaces.runtime_upgrade_detail', { runtime: space.runtime_driver }) }}
        </p>
      </template>
      <template v-else>
        <p class="font-semibold text-silver">{{ $t('spaces.runtime_update_title') }}</p>
        <p class="mt-1 text-silver" data-runtime-update-detail>{{ $t('spaces.runtime_update_detail') }}</p>
      </template>
      <template v-if="target">
        <p class="mt-1 text-storm" data-runtime-keeps>{{ $t('spaces.runtime_move_keeps') }}</p>
        <p v-if="!target.installed && !working && !running" class="mt-1 text-storm" data-runtime-download>
          {{ $t('spaces.runtime_move_download') }}
        </p>
        <Button v-if="available && !running" class="mt-3 h-auto min-h-8 py-1.5" variant="outline" size="sm" :loading="working"
                :disabled="blocked" :aria-label="copy('aria')"
                :aria-describedby="blockedReasonId" data-runtime-move-button @click="openDialog">
          {{ copy('button') }}
        </Button>
        <p v-else-if="!available" class="mt-2 text-storm" data-runtime-move-unavailable>{{ $t('spaces.runtime_move_unavailable') }}</p>
        <p v-if="otherRunning" :id="otherReasonId" class="mt-2 text-xs text-storm" data-runtime-move-other>{{ $t('spaces.runtime_move_other') }}</p>
      </template>
      <p v-else class="mt-1 text-silver" data-runtime-unpublished>
        {{ $t('spaces.runtime_move_unpublished', { host: space.host_driver, runtime: space.runtime_driver }) }}
      </p>
    </template>
    <p v-if="progress" class="mt-2 text-silver" role="status" data-runtime-progress>{{ progress }}</p>
    <p v-if="error || jobError" class="mt-2 text-warning-bright" role="alert" data-runtime-error>{{ error || jobError }}</p>
    <ConfirmActionDialog v-model="dialogOpen" :title="copy('title')"
                         :message="copy('message')"
                         :impact-items="impact" :confirm-label="$t('spaces.runtime_move_confirm')" :cancel-label="$t('spaces.cancel')"
                         :pending-label="$t('spaces.runtime_move_pending')" :pending="working"
                         :eyebrow="$t('spaces.kicker')" :impact-label="$t('spaces.dialog_impact')"
                         @confirm="move" />
  </div>
</template>

<script setup>
// A Space whose gaming runtime was made for another NVIDIA driver than the host
// runs: why it cannot start, and the move to the runtime for this driver, which
// keeps its Steam home. The host runs the move as a job; this card follows it.
import { computed, inject, onUnmounted, ref, watch } from 'vue'
import Button from './Button.vue'
import ConfirmActionDialog from './ConfirmActionDialog.vue'
import { useToast } from '../composables/useToast.js'

const props = defineProps({
  space: { type: Object, required: true },
  job: { type: Object, default: null },
  available: Boolean, locked: Boolean, ready: Boolean,
  lockReasonId: { type: String, default: '' },
  refresh: { type: Function, required: true },
  pollMs: { type: Number, default: 2000 },
})
const emit = defineEmits(['busy'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const { toast } = useToast()
const working = ref(false), error = ref(''), dialogOpen = ref(false), requestId = ref('')
// Set when this page started or watched the move, so a finished job from an
// earlier visit does not keep announcing itself.
const witnessed = ref(false)
const otherReasonId = `runtime-move-other-${Math.random().toString(36).slice(2)}`

const mismatch = computed(() => props.space.runtime_mismatch === true)
// Offered without a mismatch: the Space works today, and this runtime keeps it
// working across the next driver update.
const upgrade = computed(() => !mismatch.value && props.space.runtime_move?.reason === 'host_driver_available')
const updated = computed(() => !mismatch.value && props.space.runtime_move?.reason === 'runtime_updated')
// The words follow what the target is, not why the move is offered: a runtime
// built for one driver is named by it, the one that borrows this PC's driver
// names none, and a newer build of the same runtime is simply newer. A repair
// can land on any of them, so asking the reason here left a blank where the
// driver version should have been.
const kind = computed(() => updated.value ? 'update' : target.value?.nvidia_driver ? 'move' : 'upgrade')
const words = {
  move: { button: 'runtime_move_button', aria: 'runtime_move_aria', title: 'runtime_move_title', message: 'runtime_move_message' },
  upgrade: { button: 'runtime_upgrade_button', aria: 'runtime_upgrade_aria', title: 'runtime_upgrade_confirm_title', message: 'runtime_upgrade_confirm_message' },
  update: { button: 'runtime_update_button', aria: 'runtime_update_aria', title: 'runtime_update_confirm_title', message: 'runtime_update_confirm_message' },
}
const copy = part => t('spaces.' + words[kind.value][part], { name: props.space.name, driver: target.value?.nvidia_driver })
const target = computed(() => props.space.runtime_move?.available ? props.space.runtime_move : null)
const ownJob = computed(() => props.job?.profile_id === props.space.id ? props.job : null)
const jobState = computed(() => ownJob.value?.state || '')
const running = computed(() => ['downloading', 'moving'].includes(jobState.value))
const otherRunning = computed(() => !!props.job && !ownJob.value && ['downloading', 'moving'].includes(props.job.state))
const done = computed(() => jobState.value === 'done' && !mismatch.value && !upgrade.value && !updated.value)
const visible = computed(() => mismatch.value || upgrade.value || updated.value || running.value || (witnessed.value && !!ownJob.value))
const blocked = computed(() => props.locked || !props.ready || working.value || otherRunning.value)
const blockedReasonId = computed(() => otherRunning.value ? otherReasonId : props.locked && props.lockReasonId ? props.lockReasonId : undefined)
const impact = computed(() => [t('spaces.runtime_move_impact_home'), t('spaces.runtime_move_impact_devices'),
  ...(target.value?.installed ? [] : [t('spaces.runtime_move_impact_download')]), t('spaces.remove_impact_streams')])

// The console's words for a refusal it knows, else the host's own sentence and fix.
function refusal(result) {
  const code = typeof result?.code === 'string' ? result.code : ''
  const key = `spaces.runtime_refusal.${code}`
  const known = code ? t(key) : key
  if (known !== key) return known
  const host = [result?.message, result?.action].filter(part => typeof part === 'string' && part).join(' ')
  return host || t('spaces.runtime_move_failed')
}

const progress = computed(() => {
  const job = ownJob.value
  if (!job) return ''
  const params = { name: props.space.name, driver: job.nvidia_driver }
  if (job.state === 'downloading') return t('spaces.runtime_move_downloading', params)
  if (job.state === 'moving') return t('spaces.runtime_move_moving', params)
  if (job.state === 'done' && witnessed.value) return t('spaces.runtime_move_done', params)
  return ''
})

// The last attempt's reason stays while the Space still needs moving.
const jobError = computed(() => ownJob.value?.state === 'failed' && mismatch.value ? refusal(ownJob.value) : '')

watch(ownJob, (job, previous) => {
  if (!job) return
  if (running.value) witnessed.value = true
  if (job.state === 'done' && ['downloading', 'moving'].includes(previous?.state)) {
    error.value = ''
    toast(t('spaces.runtime_move_done', { name: props.space.name, driver: job.nvidia_driver }), 'success')
  }
}, { immediate: true })

// While the host downloads or moves, read the job back sooner than the page's own poll.
let timer = null
function stopPolling() { if (timer) { clearTimeout(timer); timer = null } }
function schedule() {
  stopPolling()
  if (!running.value) return
  timer = setTimeout(async () => {
    timer = null
    try { await props.refresh() } catch { /* The page's poll keeps trying. */ }
    schedule()
  }, props.pollMs)
}
watch(running, schedule, { immediate: true })
onUnmounted(stopPolling)

function newRequestId() {
  if (typeof globalThis.crypto?.randomUUID === 'function') return globalThis.crypto.randomUUID()
  const bytes = globalThis.crypto.getRandomValues(new Uint8Array(16))
  bytes[6] = (bytes[6] & 0x0f) | 0x40
  bytes[8] = (bytes[8] & 0x3f) | 0x80
  const hex = [...bytes].map(byte => byte.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}

function openDialog() {
  if (blocked.value || !target.value) return
  error.value = ''
  requestId.value = newRequestId()
  dialogOpen.value = true
}

async function move() {
  if (blocked.value || !target.value || !requestId.value) return
  working.value = true; emit('busy', true); error.value = ''
  try {
    const response = await fetch('./api/multiseat/profiles/runtime', { method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ request_id: requestId.value, profile_id: props.space.id, runtime_id: target.value.runtime_id }) })
    const result = await response.json().catch(() => null)
    if (result?.profile_id !== props.space.id || ![200, 202].includes(response.status) || result.status !== true)
      throw new Error(refusal(result))
    witnessed.value = true
    // 200: the Space already uses that runtime, so there is no job to follow.
    if (response.status === 200) toast(t('spaces.runtime_move_done', { name: props.space.name, driver: target.value.nvidia_driver }), 'success')
  } catch (cause) {
    error.value = cause.message || t('spaces.runtime_move_failed')
  } finally {
    working.value = false; emit('busy', false); dialogOpen.value = false
    try { await props.refresh() } catch { /* The page's poll keeps trying. */ }
  }
}
</script>
