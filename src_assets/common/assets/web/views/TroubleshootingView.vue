<template>
  <div class="page-shell pb-2">
    <section class="page-header">
      <div class="page-heading">
        <h1 class="page-title">{{ $t('troubleshooting.troubleshooting') }}</h1>
        <p class="page-subtitle">{{ $t('troubleshooting.overview') }}</p>
      </div>
      <div class="page-meta">
        <span v-if="platform" class="meta-pill">
          {{ platform }}
        </span>
        <span v-if="version" class="meta-pill">
          v{{ version }}
        </span>
      </div>
    </section>

    <section class="section-card space-y-4">
      <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
        <div>
          <div class="section-kicker">{{ $t('troubleshooting.recovery_ladder') }}</div>
          <h2 class="section-title">{{ $t('troubleshooting.quick_recovery') }}</h2>
          <p class="section-copy">{{ $t('troubleshooting.quick_recovery_desc') }}</p>
        </div>
        <div class="page-meta">
          <span class="meta-pill">{{ $t('troubleshooting.recovery_guidance') }}</span>
        </div>
      </div>
      <div class="grid gap-3 md:grid-cols-2 xl:grid-cols-4">
        <div class="surface-subtle flex h-full flex-col border-green-500/15 p-4">
          <div class="flex items-center justify-between gap-3">
            <span class="rounded-full bg-green-500/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.18em] text-green-300">{{ $t('troubleshooting.recovery_rank_1') }}</span>
            <span class="text-[10px] uppercase tracking-[0.18em] text-storm">{{ $t('troubleshooting.recovery_rank_1_label') }}</span>
          </div>
          <h3 id="close_apps" class="mt-3 text-lg font-semibold text-silver">{{ $t('troubleshooting.force_close') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.force_close_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="closeAppStatus === true">
            {{ $t('troubleshooting.force_close_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-red-500 bg-twilight/50 p-3 text-silver" v-if="closeAppStatus === false">
            {{ $t('troubleshooting.force_close_error') }}
          </div>
          <div class="mt-4">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-primary" :disabled="closeAppPressed" @click="closeApp">
              {{ $t('troubleshooting.force_close') }}
            </button>
          </div>
        </div>

        <div class="surface-subtle flex h-full flex-col border-amber-300/15 p-4">
          <div class="flex items-center justify-between gap-3">
            <span class="rounded-full bg-amber-300/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.18em] text-amber-200">{{ $t('troubleshooting.recovery_rank_2') }}</span>
            <span class="text-[10px] uppercase tracking-[0.18em] text-storm">{{ $t('troubleshooting.recovery_rank_2_label') }}</span>
          </div>
          <h3 id="restart" class="mt-3 text-lg font-semibold text-silver">{{ $t('troubleshooting.restart_polaris') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.restart_polaris_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="serverRestarting">
            {{ $t('troubleshooting.restart_polaris_success') }}
          </div>
          <div class="mt-4">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-primary" :disabled="serverQuitting || serverRestarting" @click="restart">
              {{ $t('troubleshooting.restart_polaris') }}
            </button>
          </div>
        </div>

        <div class="surface-subtle flex h-full flex-col border-red-500/15 p-4">
          <div class="flex items-center justify-between gap-3">
            <span class="rounded-full bg-red-500/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.18em] text-red-300">{{ $t('troubleshooting.recovery_rank_3') }}</span>
            <span class="text-[10px] uppercase tracking-[0.18em] text-storm">{{ $t('troubleshooting.recovery_rank_3_label') }}</span>
          </div>
          <h3 id="quit" class="mt-3 text-lg font-semibold text-silver">{{ $t('troubleshooting.quit_polaris') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.quit_polaris_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="serverQuit">
            {{ $t('troubleshooting.quit_polaris_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="serverQuitting">
            {{ $t('troubleshooting.quit_polaris_success_ongoing') }}
          </div>
          <div class="mt-4">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-danger" :disabled="serverQuitting || serverRestarting" @click="quit">
              {{ $t('troubleshooting.quit_polaris') }}
            </button>
          </div>
        </div>

        <div class="surface-subtle flex h-full flex-col border-sky-400/15 p-4" v-if="platform === 'windows'">
          <div class="flex items-center justify-between gap-3">
            <span class="rounded-full bg-sky-400/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.18em] text-sky-200">Optional</span>
            <span class="text-[10px] uppercase tracking-[0.18em] text-storm">Display reset</span>
          </div>
          <h3 id="dd_reset" class="mt-3 text-lg font-semibold text-silver">{{ $t('troubleshooting.dd_reset') }}</h3>
          <p class="mt-2 flex-1 whitespace-pre-line text-sm text-storm">{{ $t('troubleshooting.dd_reset_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver" v-if="ddResetStatus === true">
            {{ $t('troubleshooting.dd_reset_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-red-500 bg-twilight/50 p-3 text-silver" v-if="ddResetStatus === false">
            {{ $t('troubleshooting.dd_reset_error') }}
          </div>
          <div class="mt-4">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary" :disabled="ddResetPressed" @click="ddResetPersistence">
              {{ $t('troubleshooting.dd_reset') }}
            </button>
          </div>
        </div>
      </div>
    </section>

    <section class="grid gap-4 xl:grid-cols-[minmax(0,1.3fr)_minmax(320px,0.9fr)]">
      <div class="section-card">
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

      <div class="section-card">
        <h2 id="diagnostics" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.diagnostics') }}</h2>
        <p class="mt-2 text-sm text-storm">{{ $t('troubleshooting.diagnostics_desc') }}</p>
        <div v-if="groupedRecentIssues.length" class="mt-4 rounded-xl border border-storm/20 bg-deep/40 p-4">
          <div class="flex items-center justify-between gap-3">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.recent_incidents') }}</div>
            <div class="text-xs text-storm">{{ groupedRecentIssues.length }} {{ $t('troubleshooting.visible_now') }}</div>
          </div>
          <div class="mt-3 space-y-2">
            <div v-for="(entry, index) in groupedRecentIssues" :key="`${entry.level}-${entry.message}-${index}`" class="rounded-lg border border-storm/15 bg-void/40 px-3 py-2">
              <div class="flex items-start gap-3">
                <span class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.18em]"
                      :class="entry.level === 'Fatal'
                        ? 'border border-red-500/30 bg-red-500/10 text-red-200'
                        : entry.level === 'Warning'
                          ? 'border border-amber-300/30 bg-amber-300/10 text-amber-200'
                          : 'border border-storm/30 bg-deep/60 text-storm'">
                  {{ entry.level }}
                </span>
                <div class="min-w-0 flex-1">
                  <div class="text-sm text-silver break-words">{{ entry.message }}</div>
                  <div class="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-storm">
                    <span>{{ entry.timestamp }}</span>
                    <span v-if="entry.count > 1" class="rounded-full border border-storm/20 bg-void/50 px-2 py-0.5">{{ entry.count }}×</span>
                  </div>
                </div>
              </div>
            </div>
          </div>
        </div>
        <div v-else class="mt-4 rounded-xl border border-dashed border-storm/25 bg-deep/35 px-4 py-5 text-sm text-storm">
          {{ $t('troubleshooting.recent_incidents_empty') }}
        </div>
        <div class="mt-4 grid gap-2 sm:grid-cols-2">
          <button class="focus-ring troubleshooting-action-card" @click="copyRecentIssues">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.copy_recent_issues') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.copy_recent_issues_desc') }}</div>
          </button>
          <button class="focus-ring troubleshooting-action-card" :disabled="downloadingSupportBundle" @click="downloadSupportBundle">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.download_support_bundle') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.download_support_bundle_desc') }}</div>
          </button>
          <button class="focus-ring troubleshooting-action-card" :disabled="clearingAiCache" @click="clearAiCache">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.clear_ai_cache') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.clear_ai_cache_desc') }}</div>
          </button>
          <button v-if="platform === 'linux'" class="focus-ring troubleshooting-action-card" :disabled="cleaningStaleVirtualDisplay" @click="cleanupStaleVirtualDisplay">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.cleanup_stale_virtual_display') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.cleanup_stale_virtual_display_desc') }}</div>
          </button>
        </div>
      </div>
    </section>

    <section class="section-card">
      <div class="flex flex-col gap-3 xl:flex-row xl:items-end xl:justify-between">
        <div>
          <h2 id="logs" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.logs') }}</h2>
          <p class="mt-2 text-sm text-storm">{{ $t('troubleshooting.logs_desc') }}</p>
        </div>
        <div class="flex w-full flex-col gap-3 xl:w-auto xl:items-end">
          <div class="page-meta">
            <span class="meta-pill">{{ logFilterSummary }}</span>
          </div>
          <div class="flex flex-wrap items-center gap-2">
            <button v-for="level in ['All', 'Error', 'Warning', 'Fatal']" :key="level"
                    @click="logLevelFilter = level === 'All' ? null : level"
                    class="focus-ring troubleshooting-filter-button"
                    :class="(logLevelFilter === level || (level === 'All' && !logLevelFilter))
                      ? 'is-active'
                      : ''">
              {{ level }}
            </button>
          </div>
          <div class="flex w-full flex-col gap-2 sm:flex-row xl:w-auto">
            <input
              type="text"
              name="logs-filter"
              autocomplete="off"
              :aria-label="$t('troubleshooting.logs_find')"
              class="w-full rounded-lg border border-storm/50 bg-deep px-3 py-1.5 text-sm text-silver focus:border-ice focus:outline-none sm:w-72"
              v-model="logFilter"
              :placeholder="$t('troubleshooting.logs_find')"
            >
            <button
              class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary"
              :disabled="clearingLogs"
              @click="clearLogs"
            >
              {{ $t('troubleshooting.logs_clear') }}
            </button>
          </div>
        </div>
      </div>

        <div v-if="logsLoading" class="mt-4 flex h-[320px] items-center justify-center rounded-xl border border-dashed border-storm/30 bg-deep/40 px-6 text-center">
          <div>
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.logs_loading') }}</div>
            <div class="mt-2 text-sm text-storm">{{ $t('troubleshooting.logs_empty_hint') }}</div>
          </div>
        </div>
        <div v-else-if="hasFilteredLogs" class="mt-4">
          <VirtualLogViewer :logs="actualLogs" :containerHeight="320" @copy="copyLogs" />
        </div>
        <div v-else class="mt-4 flex h-[320px] items-center justify-center rounded-xl border border-dashed border-storm/30 bg-deep/40 px-6 text-center">
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

function hasRuntimeOverride(s) {
  const effectiveKnown = s.runtime_effective_headless !== undefined && s.runtime_effective_headless !== null
  return Boolean(s.runtime_requested_headless) &&
    effectiveKnown &&
    !Boolean(s.runtime_effective_headless) &&
    Boolean(s.runtime_gpu_native_override_active)
}

function runtimeModeDescription(s) {
  if (s.stream_display_mode) return s.stream_display_mode
  return `${yesNo(s.runtime_effective_headless)} effective / ${yesNo(s.runtime_requested_headless)} requested`
}

function runtimeOverrideDescription(s) {
  if (hasRuntimeOverride(s)) {
    return 'GPU-native override active: requested headless, running windowed labwc'
  }
  return s.runtime_gpu_native_override_active ? 'GPU-native override active' : 'None'
}

function captureReasonDescription(reason) {
  const key = String(reason || '').toLowerCase()
  const messages = {
    gpu_native: 'Capture and encoder conversion are GPU-resident.',
    headless_extcopy_dmabuf: 'True-headless DMA-BUF capture is active; frames stay GPU-resident through the encoder path.',
    windowed_dmabuf_override: 'Windowed private compositor is preserving the DMA-BUF/CUDA capture path.',
    headless_shm_fallback: 'Headless Stream is using SHM/system-memory capture; healthy streams can still show this, but high-FPS NVIDIA testing needs the GPU-native path.',
    headless_shm_default: 'Headless Stream is using SHM/system-memory capture; healthy streams can still show this, but high-FPS NVIDIA testing needs the GPU-native path.',
    gpu_native_requested_shm_fallback: 'GPU-native capture was requested, but Wayland capture fell back to SHM/system-memory frames.',
    gpu_native_requested_cpu_capture: 'GPU-native capture was requested, but capture frames are CPU-resident.',
    gpu_native_requested_cpu_encode_upload: 'GPU-native capture was requested, but encoder upload/conversion is CPU-resident.',
    encoder_upload_cpu: 'Capture is GPU-resident, but encoder upload/conversion crosses system memory.',
    cpu_capture: 'The active capture path is CPU-resident.',
    shm_capture: 'The active capture path is CPU-resident.',
    dmabuf_gpu_capture: 'Capture is using DMA-BUF/GPU frames, but the encoder path is not fully GPU-native.',
    no_capture_metadata: 'No capture metadata has been reported yet.',
  }
  return messages[key] || 'The active capture and encoder path is mixed or not fully classified.'
}

function fpsTargetGapDescription(s) {
  const encoded = Number(s.fps)
  const target = Number(s.session_target_fps || s.requested_client_fps)

  if (!Number.isFinite(encoded) || !Number.isFinite(target) || target < 90 || encoded <= 0) {
    return 'None'
  }

  if (encoded >= target * 0.85) {
    return 'None'
  }

  return `${formatFps(encoded)} encoded against ${formatFps(target)} target`
}

function normalizeIssueLine(line) {
  return line.replace(/^\[[^\]]+\]:\s*/, '').replace(/^\w+:\s*/, '').replace(/\s+/g, ' ').trim()
}

function lineLevel(line) {
  if (line.includes('Fatal:')) return 'Fatal'
  if (line.includes('Warning:')) return 'Warning'
  return 'Error'
}

function lineTimestamp(line) {
  const match = line.match(/^\[([^\]]+)\]/)
  return match ? `[${match[1]}]` : ''
}

const groupedRecentIssues = computed(() => {
  const grouped = new Map()

  ;(logs.value || '')
    .split('\n')
    .filter(line => /(Error|Warning|Fatal):/.test(line))
    .slice(-300)
    .reverse()
    .forEach((line) => {
      const level = lineLevel(line)
      const message = normalizeIssueLine(line)
      const key = `${level}:${message}`

      if (!grouped.has(key)) {
        grouped.set(key, {
          level,
          message,
          timestamp: lineTimestamp(line),
          count: 1
        })
        return
      }

      grouped.get(key).count += 1
    })

  return Array.from(grouped.values()).slice(0, 8)
})

const recentIssueSummaryText = computed(() => groupedRecentIssues.value
  .map((entry) => `${entry.timestamp} ${entry.level}: ${entry.message}${entry.count > 1 ? ` (${entry.count}x)` : ''}`.trim())
  .join('\n'))

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
    { label: 'Stream Display Mode', value: runtimeModeDescription(s) },
    { label: 'Runtime Override', value: runtimeOverrideDescription(s) },
    { label: 'FPS Target Gap', value: fpsTargetGapDescription(s) },
    { label: 'Capture Path', value: s.capture_path || 'unknown' },
    { label: 'Capture Reason', value: captureReasonDescription(s.capture_path_reason) },
    { label: 'Capture Transport', value: `${s.capture_transport || 'unknown'} / ${s.capture_residency || 'unknown'} / ${s.capture_format || 'unknown'}` },
    { label: 'Encode Target', value: `${s.encode_target_device || 'unknown'} / ${s.encode_target_residency || 'unknown'} / ${s.encode_target_format || 'unknown'}` },
    { label: 'GPU Native', value: yesNo(s.capture_gpu_native) },
    { label: 'CPU Copy', value: yesNo(s.capture_cpu_copy) },
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

const logsLoading = computed(() => logs.value === 'Loading...')
const hasFilteredLogs = computed(() => actualLogs.value.trim().length > 0)
const logFilterSummary = computed(() => {
  const parts = []
  parts.push(logLevelFilter.value ? `${logLevelFilter.value} only` : i18n.t('troubleshooting.logs_filter_all'))
  if (logFilter.value) {
    parts.push(i18n.t('troubleshooting.logs_filter_query', { query: logFilter.value }))
  } else {
    parts.push(i18n.t('troubleshooting.logs_filter_live'))
  }
  return parts.join(' · ')
})

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
  if (!actualLogs.value.trim()) {
    showToast(i18n.t('troubleshooting.copy_logs_empty') || 'No visible logs to copy.', 'info')
    return
  }
  navigator.clipboard.writeText(actualLogs.value)
  showToast(i18n.t('troubleshooting.copy_logs_success') || 'Visible log lines copied.', 'success')
}

function copyRecentIssues() {
  if (!recentIssueSummaryText.value.trim()) {
    showToast(i18n.t('troubleshooting.copy_recent_issues_empty') || 'No recent warnings or errors to copy.', 'info')
    return
  }

  navigator.clipboard.writeText(recentIssueSummaryText.value)
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
      recent_issues: groupedRecentIssues.value,
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
