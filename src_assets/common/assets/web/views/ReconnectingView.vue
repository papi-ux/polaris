<template>
  <main class="relative flex min-h-screen items-center justify-center overflow-hidden bg-background p-6">
    <div class="pointer-events-none absolute inset-0 bg-[radial-gradient(circle_at_top,rgba(124,115,255,0.16),transparent_38%),radial-gradient(circle_at_bottom_right,rgba(200,214,229,0.08),transparent_30%)]"></div>
    <section
      role="status"
      aria-live="polite"
      :aria-busy="checking"
      class="glass relative w-full max-w-lg rounded-[28px] border border-storm/30 p-8 text-center shadow-2xl"
    >
      <img :src="'/images/logo-polaris.svg'" class="mx-auto h-12" alt="Polaris">
      <div class="mt-5 text-[10px] font-semibold uppercase tracking-[0.24em] text-storm/85">Host connection</div>
      <h1 class="mt-3 text-3xl font-bold text-silver">Host reconnecting…</h1>
      <p class="mt-4 text-sm leading-relaxed text-storm">
        Polaris is temporarily unavailable. Your session has not been cleared.
      </p>
      <div class="mx-auto mt-6 flex w-fit items-center gap-3 rounded-full border border-storm/20 bg-void/45 px-4 py-2 text-xs text-storm">
        <span class="h-2.5 w-2.5 animate-pulse rounded-full bg-amber-300 shadow-[0_0_14px_rgba(252,211,77,0.55)]"></span>
        {{ checking ? 'Checking host…' : `Retrying connection · attempt ${attempts + 1}` }}
      </div>
      <button
        type="button"
        class="mt-6 inline-flex h-10 items-center justify-center rounded-xl border border-storm/30 bg-deep/55 px-4 text-sm font-semibold text-silver transition-colors hover:border-ice/40 hover:bg-deep/75 disabled:cursor-not-allowed disabled:opacity-50"
        :disabled="checking"
        @click="retryNow"
      >
        {{ checking ? 'Checking…' : 'Retry now' }}
      </button>
    </section>
  </main>
</template>

<script setup>
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import {
  markWebUiAuthenticated,
  markWebUiUnauthenticated,
} from '../auth-state.js'
import {
  AUTH_PROBE_STATE,
  clearPolarisReloadParam,
  getSafeAuthReturnTarget,
  probeWebUiAuth,
} from '../router-helpers.js'
import {
  beginWebUiAuthProbeGeneration,
  isCurrentWebUiAuthProbeGeneration,
  runWebUiAuthProbeForGeneration,
} from '../auth-probe-coordinator.js'

const RETRY_DELAYS_MS = [500, 1000, 2000, 4000, 8000]

const route = useRoute()
const router = useRouter()
const attempts = ref(0)
const checking = ref(false)
const returnTarget = computed(() => getSafeAuthReturnTarget(route.query.redirect))

let viewProbeGeneration = 0
let retryTimer = null
let stopped = false

function clearRetryTimer() {
  if (retryTimer !== null) {
    window.clearTimeout(retryTimer)
    retryTimer = null
  }
}

function scheduleRetry() {
  if (stopped) return
  const delayIndex = Math.min(Math.max(attempts.value - 1, 0), RETRY_DELAYS_MS.length - 1)
  retryTimer = window.setTimeout(checkHost, RETRY_DELAYS_MS[delayIndex])
}

async function checkHost() {
  if (stopped) return

  clearRetryTimer()
  const viewGeneration = ++viewProbeGeneration
  const target = returnTarget.value
  checking.value = true
  attempts.value += 1
  let retry = false

  const probeGeneration = beginWebUiAuthProbeGeneration()
  try {
    const outcome = await runWebUiAuthProbeForGeneration(
      probeGeneration,
      (signal) => probeWebUiAuth(window.fetch, window.location.href, signal)
    )
    if (
      stopped
      || viewGeneration !== viewProbeGeneration
      || !outcome.current
      || !isCurrentWebUiAuthProbeGeneration(probeGeneration)
    ) return

    const result = outcome.result
    if (result.state === AUTH_PROBE_STATE.authenticated) {
      markWebUiAuthenticated(result.config)
      clearPolarisReloadParam()
      await router.replace(target)
      return
    }
    if (result.state === AUTH_PROBE_STATE.login) {
      markWebUiUnauthenticated()
      await router.replace({
        path: '/login',
        query: { redirect: target },
      })
      return
    }
    if (result.state === AUTH_PROBE_STATE.welcome) {
      markWebUiUnauthenticated()
      await router.replace('/welcome')
      return
    }

    retry = true
  } catch {
    retry = true
  } finally {
    if (viewGeneration !== viewProbeGeneration) return
    checking.value = false
    if (retry) scheduleRetry()
  }
}

function retryNow() {
  if (checking.value) return
  clearRetryTimer()
  void checkHost()
}

watch(() => route.fullPath, () => {
  if (stopped) return

  clearRetryTimer()
  viewProbeGeneration += 1
  checking.value = false
  attempts.value = 0
  beginWebUiAuthProbeGeneration()
  void checkHost()
})

onMounted(() => {
  void checkHost()
})

onUnmounted(() => {
  stopped = true
  viewProbeGeneration += 1
  beginWebUiAuthProbeGeneration()
  clearRetryTimer()
})
</script>
