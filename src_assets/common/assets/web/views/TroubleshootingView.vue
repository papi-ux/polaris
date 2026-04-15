<template>
  <div class="space-y-6 pb-2">
    <div class="flex flex-col gap-3 lg:flex-row lg:items-end lg:justify-between">
      <div>
        <h1 class="text-2xl font-bold text-silver">{{ $t('troubleshooting.troubleshooting') }}</h1>
        <p class="mt-2 max-w-3xl text-sm text-storm">{{ $t('troubleshooting.overview') }}</p>
      </div>
      <div class="flex flex-wrap items-center gap-2 text-xs">
        <span class="rounded-full border px-2.5 py-1"
              :class="streamStatsConnected ? 'border-green-500/40 bg-green-500/10 text-green-300' : 'border-storm/40 bg-deep/60 text-storm'">
          {{ streamStatsConnected ? $t('troubleshooting.session_snapshot_connected') : $t('troubleshooting.session_snapshot_disconnected') }}
        </span>
        <span v-if="platform" class="rounded-full border border-storm/30 bg-deep/60 px-2.5 py-1 text-storm">
          {{ platform }}
        </span>
      </div>
    </div>

    <section class="space-y-3">
      <div>
        <h2 class="text-lg font-semibold text-silver">{{ $t('troubleshooting.quick_recovery') }}</h2>
        <p class="mt-1 text-sm text-storm">{{ $t('troubleshooting.quick_recovery_desc') }}</p>
      </div>
      <div class="grid gap-3 md:grid-cols-2 xl:grid-cols-4">
        <div class="card flex h-full flex-col p-4">
          <h3 id="close_apps" class="text-lg font-semibold text-silver">{{ $t('troubleshooting.force_close') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.force_close_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="closeAppStatus === true">
            {{ $t('troubleshooting.force_close_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-red-500 bg-twilight/50 p-3 text-silver" v-if="closeAppStatus === false">
            {{ $t('troubleshooting.force_close_error') }}
          </div>
          <div class="mt-4">
            <button class="inline-flex h-9 items-center gap-2 rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] disabled:cursor-not-allowed disabled:opacity-50" :disabled="closeAppPressed" @click="closeApp">
              {{ $t('troubleshooting.force_close') }}
            </button>
          </div>
        </div>

        <div class="card flex h-full flex-col p-4">
          <h3 id="restart" class="text-lg font-semibold text-silver">{{ $t('troubleshooting.restart_polaris') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.restart_polaris_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="serverRestarting">
            {{ $t('troubleshooting.restart_polaris_success') }}
          </div>
          <div class="mt-4">
            <button class="inline-flex h-9 items-center gap-2 rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] disabled:cursor-not-allowed disabled:opacity-50" :disabled="serverQuitting || serverRestarting" @click="restart">
              {{ $t('troubleshooting.restart_polaris') }}
            </button>
          </div>
        </div>

        <div class="card flex h-full flex-col p-4">
          <h3 id="quit" class="text-lg font-semibold text-silver">{{ $t('troubleshooting.quit_polaris') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.quit_polaris_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="serverQuit">
            {{ $t('troubleshooting.quit_polaris_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="serverQuitting">
            {{ $t('troubleshooting.quit_polaris_success_ongoing') }}
          </div>
          <div class="mt-4">
            <button class="inline-flex h-9 items-center gap-2 rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] disabled:cursor-not-allowed disabled:opacity-50" :disabled="serverQuitting || serverRestarting" @click="quit">
              {{ $t('troubleshooting.quit_polaris') }}
            </button>
          </div>
        </div>

        <div class="card flex h-full flex-col p-4" v-if="platform === 'windows'">
          <h3 id="dd_reset" class="text-lg font-semibold text-silver">{{ $t('troubleshooting.dd_reset') }}</h3>
          <p class="mt-2 flex-1 whitespace-pre-line text-sm text-storm">{{ $t('troubleshooting.dd_reset_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="ddResetStatus === true">
            {{ $t('troubleshooting.dd_reset_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-red-500 bg-twilight/50 p-3 text-silver" v-if="ddResetStatus === false">
            {{ $t('troubleshooting.dd_reset_error') }}
          </div>
          <div class="mt-4">
            <button class="inline-flex h-9 items-center gap-2 rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] disabled:cursor-not-allowed disabled:opacity-50" :disabled="ddResetPressed" @click="ddResetPersistence">
              {{ $t('troubleshooting.dd_reset') }}
            </button>
          </div>
        </div>
      </div>
    </section>

    <section class="grid gap-4 xl:grid-cols-[minmax(0,1.3fr)_minmax(320px,0.9fr)]">
      <div class="card p-4">
        <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
          <div>
            <h2 id="session_snapshot" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.session_snapshot') }}</h2>
            <p class="mt-2 text-sm text-storm">{{ $t('troubleshooting.session_snapshot_desc') }}</p>
          </div>
          <span class="rounded-full border px-2.5 py-1 text-sm"
                :class="streamStatsConnected ? 'border-green-500/40 bg-green-500/10 text-green-300' : 'border-storm/40 bg-deep/60 text-storm'">
            {{ streamStatsConnected ? $t('troubleshooting.session_snapshot_connected') : $t('troubleshooting.session_snapshot_disconnected') }}
          </span>
        </div>

        <div v-if="!streamStats" class="mt-4 rounded-xl border border-dashed border-storm/30 bg-deep/40 px-4 py-5 text-sm text-storm">
          {{ $t('troubleshooting.session_snapshot_waiting') }}
        </div>
        <div v-else-if="!streamStats.streaming" class="mt-4 rounded-xl border border-dashed border-storm/30 bg-deep/40 px-4 py-5 text-sm text-storm">
          {{ $t('troubleshooting.session_snapshot_idle') }}
        </div>
        <template v-else>
          <div class="mt-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <div v-for="item in sessionSnapshotSummaryItems" :key="item.label" class="rounded-xl border border-storm/25 bg-deep/60 px-3 py-3">
              <div class="text-[11px] uppercase tracking-wide text-storm">{{ item.label }}</div>
              <div class="mt-1 text-sm font-medium text-silver break-words">{{ item.value }}</div>
            </div>
          </div>
          <div class="mt-4 grid gap-3 sm:grid-cols-2">
            <div v-for="item in sessionSnapshotDetailItems" :key="item.label" class="rounded-xl border border-storm/20 bg-deep/40 px-3 py-2.5">
              <div class="text-[11px] uppercase tracking-wide text-storm">{{ item.label }}</div>
              <div class="mt-1 text-sm font-medium text-silver break-words">{{ item.value }}</div>
            </div>
          </div>
        </template>
      </div>

      <div class="card p-4">
        <h2 id="diagnostics" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.diagnostics') }}</h2>
        <p class="mt-2 text-sm text-storm">{{ $t('troubleshooting.diagnostics_desc') }}</p>
        <div class="mt-4 grid gap-2 sm:grid-cols-2">
          <button class="rounded-xl border border-storm/30 bg-deep/40 p-3 text-left transition-all duration-200 hover:border-ice hover:text-ice disabled:cursor-not-allowed disabled:opacity-50" @click="copyRecentIssues">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.copy_recent_issues') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.copy_recent_issues_desc') }}</div>
          </button>
          <button class="rounded-xl border border-storm/30 bg-deep/40 p-3 text-left transition-all duration-200 hover:border-ice hover:text-ice disabled:cursor-not-allowed disabled:opacity-50" :disabled="downloadingSupportBundle" @click="downloadSupportBundle">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.download_support_bundle') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.download_support_bundle_desc') }}</div>
          </button>
          <button class="rounded-xl border border-storm/30 bg-deep/40 p-3 text-left transition-all duration-200 hover:border-ice hover:text-ice disabled:cursor-not-allowed disabled:opacity-50" :disabled="clearingAiCache" @click="clearAiCache">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.clear_ai_cache') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.clear_ai_cache_desc') }}</div>
          </button>
          <button v-if="platform === 'linux'" class="rounded-xl border border-storm/30 bg-deep/40 p-3 text-left transition-all duration-200 hover:border-ice hover:text-ice disabled:cursor-not-allowed disabled:opacity-50" :disabled="cleaningStaleVirtualDisplay" @click="cleanupStaleVirtualDisplay">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.cleanup_stale_virtual_display') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.cleanup_stale_virtual_display_desc') }}</div>
          </button>
        </div>
      </div>
    </section>

    <section class="card p-4">
      <div class="flex flex-col gap-3 xl:flex-row xl:items-end xl:justify-between">
        <div>
          <h2 id="logs" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.logs') }}</h2>
          <p class="mt-2 text-sm text-storm">{{ $t('troubleshooting.logs_desc') }}</p>
        </div>
        <div class="flex w-full flex-col gap-3 xl:w-auto xl:items-end">
          <div class="flex flex-wrap items-center gap-2">
            <button v-for="level in ['All', 'Error', 'Warning', 'Fatal']" :key="level"
                    @click="logLevelFilter = level === 'All' ? null : level"
                    class="h-7 rounded-md px-2.5 text-xs font-medium transition-all duration-150"
                    :class="(logLevelFilter === level || (level === 'All' && !logLevelFilter))
                      ? 'border border-ice/30 bg-ice/15 text-ice'
                      : 'border border-storm/30 text-storm hover:border-storm hover:text-silver'">
              {{ level }}
            </button>
          </div>
          <div class="flex w-full flex-col gap-2 sm:flex-row xl:w-auto">
            <input type="text" class="w-full rounded-lg border border-storm/50 bg-deep px-3 py-1.5 text-sm text-silver focus:border-ice focus:outline-none sm:w-72" v-model="logFilter" :placeholder="$t('troubleshooting.logs_find')">
            <button
              class="h-8 rounded-md border border-storm/30 px-3 text-xs font-medium text-storm transition-all duration-150 hover:border-storm hover:text-silver disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="clearingLogs"
              @click="clearLogs"
            >
              {{ $t('troubleshooting.logs_clear') }}
            </button>
          </div>
        </div>
      </div>

      <div v-if="hasFilteredLogs" class="mt-4">
        <VirtualLogViewer :logs="actualLogs" :containerHeight="400" @copy="copyLogs" />
      </div>
      <div v-else class="mt-4 flex h-[400px] items-center justify-center rounded-xl border border-dashed border-storm/30 bg-deep/40 px-6 text-center">
        <div>
          <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.logs_empty') }}</div>
          <div class="mt-2 text-sm text-storm">{{ $t('troubleshooting.logs_empty_hint') }}</div>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup>
import { ref, computed, onBeforeUnmount, inject } from 'vue'
import { useToast } from '../composables/useToast'
import { useStreamStats } from '../composables/useStreamStats'
import VirtualLogViewer from '../components/VirtualLogViewer.vue'

const { toast: showToast } = useToast()
const i18n = inject('i18n')
const { stats: streamStats, connected: streamStatsConnected } = useStreamStats()

const closeAppPressed = ref(false)
const closeAppStatus = ref(null)
const ddResetPressed = ref(false)
const ddResetStatus = ref(null)
const logs = ref('Loading...')
const clearingLogs = ref(false)
const clearingAiCache = ref(false)
const cleaningStaleVirtualDisplay = ref(false)
const downloadingSupportBundle = ref(false)
const logFilter = ref(null)
const logLevelFilter = ref(null)
let logInterval = null
const serverRestarting = ref(false)
const serverQuitting = ref(false)
const serverQuit = ref(false)
const platform = ref("")
const version = ref("")

function yesNo(value) {
  return value ? 'Yes' : 'No'
}

function formatNumber(value, digits = 1) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) {
    return '0'
  }
  return Number(value).toFixed(digits)
}

function formatFps(value) {
  return `${formatNumber(value, 1)} FPS`
}

function formatResolution(width, height) {
  if (!width || !height) return '(unknown)'
  return `${width}x${height}`
}

const recentIssueLines = computed(() => (
  (logs.value || '')
    .split('\n')
    .filter(line => /(Error|Warning|Fatal):/.test(line))
    .slice(-200)
    .join('\n')
))

const sessionSnapshotItems = computed(() => {
  if (!streamStats.value || !streamStats.value.streaming) return []

  const s = streamStats.value
  return [
    { label: 'Resolution', value: formatResolution(s.width, s.height) },
    { label: 'Codec', value: s.codec || '(unknown)' },
    { label: 'FPS', value: `${formatFps(s.fps)} encoded / ${formatFps(s.session_target_fps)} target` },
    { label: 'Bitrate', value: `${s.bitrate_kbps || 0} kbps` },
    { label: 'Client IP', value: s.client_ip || '(unknown)' },
    { label: 'Active Sessions', value: `${s.active_sessions ?? 0}` },
    { label: 'Requested FPS', value: formatFps(s.requested_client_fps) },
    { label: 'Runtime Backend', value: s.runtime_backend || '(unknown)' },
    { label: 'Headless', value: `${yesNo(s.runtime_effective_headless)} effective / ${yesNo(s.runtime_requested_headless)} requested` },
    { label: 'Capture Path', value: `${s.capture_transport || 'unknown'} / ${s.capture_residency || 'unknown'} / ${s.capture_format || 'unknown'}` },
    { label: 'Pacing Policy', value: s.pacing_policy || 'none' },
    { label: 'Optimization Source', value: s.optimization_source || 'default' },
    { label: 'Network', value: `${formatNumber(s.latency_ms, 1)} ms latency / ${formatNumber(s.packet_loss, 2)}% loss` },
    { label: 'Frame Delivery', value: `${formatNumber((s.duplicate_frame_ratio || 0) * 100, 2)}% duplicate / ${formatNumber((s.dropped_frame_ratio || 0) * 100, 2)}% dropped` },
    { label: 'Frame Timing', value: `${formatNumber(s.avg_frame_age_ms, 2)} ms age / ${formatNumber(s.frame_jitter_ms, 2)} ms jitter` }
  ]
})

const sessionSnapshotSummaryItems = computed(() => {
  if (!streamStats.value || !streamStats.value.streaming) return []
  return [
    { label: 'Client', value: streamStats.value.client_name || '(unknown)' },
    ...sessionSnapshotItems.value.slice(0, 3)
  ]
})

const sessionSnapshotDetailItems = computed(() => {
  if (!streamStats.value || !streamStats.value.streaming) return []
  return sessionSnapshotItems.value.slice(3)
})

const actualLogs = computed(() => {
  if (!logFilter.value && !logLevelFilter.value) return logs.value
  let lines = logs.value.split("\n")
  if (logLevelFilter.value) {
    lines = lines.filter(x => x.includes(logLevelFilter.value + ':'))
  }
  if (logFilter.value) {
    lines = lines.filter(x => x.indexOf(logFilter.value) !== -1)
  }
  return lines.join("\n")
})

const hasFilteredLogs = computed(() => actualLogs.value.trim().length > 0)

function refreshLogs() {
  fetch("./api/logs", { credentials: 'include' })
    .then(response => {
      const contentType = response.headers.get("Content-Type") || ""
      const charsetMatch = contentType.match(/charset=([^;]+)/i)
      const charset = charsetMatch ? charsetMatch[1].trim() : "utf-8"
      return response.arrayBuffer().then(buffer => {
        const decoder = new TextDecoder(charset)
        return decoder.decode(buffer)
      })
    })
    .then(text => {
      logs.value = text
    })
    .catch(error => console.error("Error fetching logs:", error))
}

function closeApp() {
  closeAppPressed.value = true
  fetch("./api/apps/close", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      closeAppPressed.value = false
      closeAppStatus.value = r.status
      setTimeout(() => { closeAppStatus.value = null }, 5000)
    })
}

function copyLogs() {
  navigator.clipboard.writeText(actualLogs.value)
}

function copyRecentIssues() {
  if (!recentIssueLines.value.trim()) {
    showToast(i18n.t('troubleshooting.copy_recent_issues_empty') || 'No recent warnings or errors to copy.', 'info')
    return
  }

  navigator.clipboard.writeText(recentIssueLines.value)
  showToast(i18n.t('troubleshooting.copy_recent_issues_success') || 'Recent warnings and errors copied.', 'success')
}

function clearLogs() {
  clearingLogs.value = true
  fetch("./api/logs/clear", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      if (!r.status) {
        throw new Error(r.error || "Failed to clear logs")
      }

      logs.value = ''
      showToast(i18n.t('troubleshooting.logs_clear_success') || 'Logs cleared.', 'success')
      refreshLogs()
    })
    .catch((error) => {
      console.error(error)
      showToast(i18n.t('troubleshooting.logs_clear_error') || 'Failed to clear logs.', 'error')
    })
    .finally(() => {
      clearingLogs.value = false
    })
}

async function safeFetchJson(url) {
  try {
    const response = await fetch(url, { credentials: 'include' })
    if (!response.ok) {
      return { _error: `HTTP ${response.status}` }
    }
    return await response.json()
  } catch (error) {
    return { _error: error instanceof Error ? error.message : String(error) }
  }
}

async function safeFetchText(url) {
  try {
    const response = await fetch(url, { credentials: 'include' })
    if (!response.ok) {
      return `HTTP ${response.status}`
    }
    return await response.text()
  } catch (error) {
    return error instanceof Error ? error.message : String(error)
  }
}

function triggerDownload(filename, content, mimeType) {
  const blob = new Blob([content], { type: mimeType })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = filename
  document.body.appendChild(link)
  link.click()
  document.body.removeChild(link)
  URL.revokeObjectURL(url)
}

function clearAiCache() {
  clearingAiCache.value = true
  fetch("./api/ai/cache/clear", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      if (!r.status) {
        throw new Error(r.error || 'Failed to clear AI cache')
      }

      showToast(i18n.t('troubleshooting.clear_ai_cache_success') || 'AI optimization cache cleared.', 'success')
    })
    .catch((error) => {
      console.error(error)
      showToast(i18n.t('troubleshooting.clear_ai_cache_error') || 'Failed to clear AI optimization cache.', 'error')
    })
    .finally(() => {
      clearingAiCache.value = false
    })
}

function cleanupStaleVirtualDisplay() {
  cleaningStaleVirtualDisplay.value = true
  fetch("./api/virtual-display/cleanup-stale", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      if (!r.status) {
        throw new Error(r.error || 'Failed to clean stale virtual display state')
      }

      if (r.cleaned) {
        showToast(i18n.t('troubleshooting.cleanup_stale_virtual_display_success') || 'Stale virtual display state cleaned up.', 'success')
      } else {
        showToast(i18n.t('troubleshooting.cleanup_stale_virtual_display_none') || 'No stale virtual display state was found.', 'info')
      }
    })
    .catch((error) => {
      console.error(error)
      showToast(i18n.t('troubleshooting.cleanup_stale_virtual_display_error') || 'Failed to clean stale virtual display state.', 'error')
    })
    .finally(() => {
      cleaningStaleVirtualDisplay.value = false
    })
}

async function downloadSupportBundle() {
  downloadingSupportBundle.value = true

  try {
    const [systemStats, aiStatus, aiCache, aiHistory, latestLogs] = await Promise.all([
      safeFetchJson('./api/stats/system'),
      safeFetchJson('./api/ai/status'),
      safeFetchJson('./api/ai/cache'),
      safeFetchJson('./api/ai/history'),
      safeFetchText('./api/logs')
    ])

    const bundle = {
      generated_at: new Date().toISOString(),
      platform: platform.value || 'unknown',
      version: version.value || 'unknown',
      browser_user_agent: navigator.userAgent,
      stream_stats_connected: streamStatsConnected.value,
      session_snapshot: streamStats.value,
      system_stats: systemStats,
      ai_status: aiStatus,
      ai_cache: aiCache,
      ai_history: aiHistory,
      recent_issues: recentIssueLines.value ? recentIssueLines.value.split('\n') : [],
      logs: latestLogs || logs.value
    }

    const timestamp = new Date().toISOString().replace(/[:]/g, '-')
    triggerDownload(`polaris-support-bundle-${timestamp}.json`, JSON.stringify(bundle, null, 2), 'application/json;charset=utf-8')
    showToast(i18n.t('troubleshooting.download_support_bundle_success') || 'Support bundle downloaded.', 'success')
  } catch (error) {
    console.error(error)
    showToast(i18n.t('troubleshooting.download_support_bundle_error') || 'Failed to build support bundle.', 'error')
  } finally {
    downloadingSupportBundle.value = false
  }
}

function restart() {
  serverRestarting.value = true
  setTimeout(() => { serverRestarting.value = false }, 5000)
  fetch("./api/restart", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
  .then((resp) => {
    if (resp.status !== 200) {
      setTimeout(() => { location.reload() }, 1000)
      return
    }
  })
  .catch((e) => {
    serverRestarting.value = false
    console.error(e)
    setTimeout(() => { location.reload() }, 1000)
  })
}

function quit() {
  if (window.confirm(i18n.t('troubleshooting.quit_polaris_confirm'))) {
    serverQuitting.value = true
    fetch("./api/quit", {
      credentials: 'include',
      method: 'POST',
      headers: { 'Content-Type': 'application/json' }
    })
    .then(() => {
      serverQuitting.value = false
      serverQuit.value = false
      showToast("Exit failed!", 'error')
    })
    .catch(() => {
      serverQuitting.value = false
      serverQuit.value = true
    })
  }
}

function ddResetPersistence() {
  ddResetPressed.value = true
  fetch("/api/reset-display-device-persistence", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      ddResetPressed.value = false
      ddResetStatus.value = r.status
      setTimeout(() => { ddResetStatus.value = null }, 5000)
    })
}

// created() logic
fetch("/api/config")
  .then((r) => r.json())
  .then((r) => {
    platform.value = r.platform
    version.value = r.version || ''
  })

logInterval = setInterval(() => { refreshLogs() }, 5000)
refreshLogs()

onBeforeUnmount(() => {
  clearInterval(logInterval)
})
</script>
