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
            <span class="control-chip">{{ readyCount }}/{{ countedChecks.length }}</span>
            <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
              <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7" />
            </svg>
          </span>
        </summary>
        <ol class="mt-3 grid gap-3 md:grid-cols-2">
          <li v-for="check in setup.checks" :key="check.id" class="min-w-0 rounded-xl border bg-deep/40 p-4"
              :class="waitingCheck(check) ? 'border-storm/20' : statusTone(checkStatus(check)).card" :data-setup-check="check.id">
            <div class="flex flex-wrap items-center justify-between gap-2">
              <h3 class="text-sm font-semibold text-silver">{{ check.title }}</h3>
              <span v-if="waitingCheck(check)" class="meta-pill border border-storm/30 bg-deep/40 text-storm" data-check-waiting>
                {{ $t('spaces.check_waiting_runtime') }}
              </span>
              <StatusBadge v-else :status="checkStatus(check)" :label="checkLabel(check)" />
            </div>
            <p class="mt-2 text-sm text-storm">{{ waitingCheck(check) ? $t('spaces.check_waiting_runtime_detail') : check.detail }}</p>
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
              {{ $t('spaces.spaces_hint') }} {{ $t('spaces.spaces_hint_runtime') }}
            </p>
          </li>
        </ol>
      </details>
      <p v-if="!setup.available" class="mt-4 text-xs text-storm">{{ $t('spaces.no_mutation') }}</p>
      <SpacesFirstSetup v-if="!setup.configured" id="spaces-prepare" :host-ready="setup.host_prerequisites_ready" @runtime="runtime = $event" />
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

const emit = defineEmits(['state', 'runtime-waiting'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const setup = ref(null), loading = ref(false), error = ref(''), copied = ref('')
// Both disclosures open themselves when something needs attention and never
// close under the reader: a successful recheck leaves the page where it was.
const open = ref(true), checksOpen = ref(true)
let request, seen = false, copyTimer
onUnmounted(() => { request?.abort(); clearTimeout(copyTimer) })

// What the first-Space setup last heard from the host about the runtime download.
const runtime = ref(null)
// A build with no verified gaming runtime cannot create a Space, and nothing on
// this PC can change that. The Spaces configuration check then waits instead of
// asking for attention, and the count covers only the checks a person can act on.
const runtimeWaiting = computed(() => !!setup.value && !setup.value.configured &&
  runtime.value?.available === false && runtime.value?.reason === 'runtime_not_published')
watch(runtimeWaiting, waiting => emit('runtime-waiting', waiting))
const waitingCheck = check => runtimeWaiting.value && check.id === 'spaces' && check.state === 'not_configured'
const countedChecks = computed(() => (setup.value?.checks || []).filter(check => !waitingCheck(check)))
const readyCount = computed(() => countedChecks.value.filter(check => check.state === 'ready').length)
const hostReadyAndWaiting = computed(() => runtimeWaiting.value && setup.value?.host_prerequisites_ready)
const summaryTone = computed(() => error.value ? 'fail' : setup.value?.available && setup.value?.host_prerequisites_ready ? 'pass' : setup.value?.configured ? 'fail' : hostReadyAndWaiting.value ? 'pass' : 'warning')
const summaryLabel = computed(() => error.value ? t('spaces.host_attention') :
  setup.value?.available && setup.value?.host_prerequisites_ready ? t('spaces.host_configured') :
  setup.value?.configured ? t('spaces.host_attention') : hostReadyAndWaiting.value ? t('spaces.host_ready_badge') : t('spaces.host_set_up'))
const checkStatus = check => check.state === 'ready' ? 'pass' : check.state === 'not_configured' ? 'warning' : 'fail'
const checkLabel = check => t(check.state === 'ready' ? 'spaces.check_ready' : check.state === 'not_configured' ? 'spaces.check_not_configured' : 'spaces.check_required')
const guideLabel = check => t(check.id === 'security' ? 'spaces.guide_security' : ['docker', 'docker_access'].includes(check.id) ? 'spaces.guide_docker' : 'spaces.guide_section')
const stepsFor = check => setupSteps(setup.value, check)

function settle(next) {
  const needsAttention = !next.available || !next.host_prerequisites_ready
  const checksNeedAttention = !next.host_prerequisites_ready || (next.configured && !next.available)
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
  } catch (cause) {
    setup.value = null
    open.value = true
    error.value = cause.name === 'AbortError' ? t('spaces.setup_timeout') : cause.message || t('spaces.setup_failed')
    emit('state', null)
  } finally { clearTimeout(timeout); loading.value = false }
}
onMounted(refresh)
</script>
