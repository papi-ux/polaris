<script setup>
import { ref, computed, inject, watch, onMounted } from 'vue'
import { $tp } from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import VirtualDisplayStatus from "./audiovideo/VirtualDisplayStatus.vue";
import Checkbox from "../../Checkbox.vue";
import SelectableCard from '../../components/SelectableCard.vue'
import StatTile from '../../components/StatTile.vue'
import {
  applyStreamDisplayModeToConfig,
  resolveClientSettingsSync,
  resolveStreamDisplayMode,
  resolveStreamDisplayModeAvailability,
  resolveStreamDisplayRuntimeNotice,
} from '../../client-settings-sync'
import { buildResolutionPlanner } from '../../display-resolution-planner'
import { provenanceLabel, useConfigProjection } from '../../composables/useConfigProjection'
import { useStreamStats } from '../../composables/useStreamStats'
import {
  autoQualityHostStateKey,
  autoQualityHostTone,
  buildLiveAutoQualityRows,
} from '../../auto-quality-live'

const $t = inject('i18n').t;

const props = defineProps([
  'platform',
  'config',
  'vdisplay',
  'min_fps_factor',
])

const sudovdaStatus = {
  '1': 'Unknown',
  '0': 'Ready',
  '-1': 'Uninitialized',
  '-2': 'Version Incompatible',
  '-3': 'Watchdog Failed'
}

const currentDriverStatus = computed(() => sudovdaStatus[props.vdisplay])
const config = ref(props.config)

// Host truth for this page. Mode badges and availability, stream-display
// labels, field provenance, and the Auto Quality strip read the projection
// when the host serves it; every consumer keeps its config-derived fallback
// for hosts that answer 404. Live tuning rides the stats channel at 1 Hz.
const projection = useConfigProjection()
onMounted(() => {
  projection.load()
})
const liveStreamStats = typeof window !== 'undefined' && typeof window.EventSource === 'function'
  ? useStreamStats(2000, { pauseWhenHidden: true }).stats
  : ref(null)
const projectionModes = computed(() => (
  projection.ok.value && Array.isArray(projection.modes.value) ? projection.modes.value : null
))
const hostStreamDisplay = computed(() => (projection.ok.value ? projection.streamDisplay.value : null))
const liveTuning = computed(() => liveStreamStats.value?.tuning || projection.tuning.value || null)
const liveAutoQuality = computed(() => liveStreamStats.value?.auto_quality || projection.autoQuality.value || null)
const provenanceFor = (configKey) => provenanceLabel(projection, configKey)
const isLinux = computed(() => props.platform === 'linux')
const isWindows = computed(() => props.platform === 'windows')
const showDisplayPlannerAdvanced = ref(false)
const customDisplayScale = ref(1)
const plannerSourceMode = ref(config.value.fallback_mode || '')
let applyingDisplayPlan = false
const displayPlanner = computed(() => buildResolutionPlanner({
  sourceMode: plannerSourceMode.value,
  fallbackMode: config.value.fallback_mode,
  customScale: customDisplayScale.value,
  showAdvanced: showDisplayPlannerAdvanced.value,
}))
// A plan is active when its persisted id still matches the saved fallback mode;
// a hand-edited fallback mode clears the id, so a stale pairing never lights up.
const PERSISTABLE_PLAN_IDS = ['native', 'balanced', 'sharp', 'performance']
const activeDisplayPlanId = computed(() => {
  const current = String(config.value.fallback_mode || '').trim()
  if (!current) {
    return ''
  }
  const savedPlan = String(config.value.display_plan || '').trim()
  if (savedPlan) {
    const saved = displayPlanner.value.choices.find((choice) => choice.id === savedPlan)
    if (saved && saved.targetMode === current) {
      return saved.id
    }
  }
  const match = displayPlanner.value.choices.find((choice) => choice.targetMode === current)
  return match ? match.id : ''
})

watch(() => config.value.fallback_mode, (mode) => {
  if (applyingDisplayPlan) {
    applyingDisplayPlan = false
    return
  }
  plannerSourceMode.value = mode || ''
  // A manual fallback-mode edit means the user left the plan.
  if (config.value.display_plan) {
    config.value.display_plan = ''
  }
}, { flush: 'sync' })

// Primary copy answers the player's question first. Backend vocabulary stays in the
// technical disclosure, where it remains available for diagnosis without making
// labwc/wlroots/portal knowledge a prerequisite for choosing a mode.
// The copy itself lives in the locale files under config.av_mode_*.
const streamDisplayModeDefinitions = [
  {
    id: 'headless_stream',
    title: $t('config.av_mode_headless_stream_title'),
    badge: $t('config.av_mode_headless_stream_badge'),
    group: 'private',
    copy: $t('config.av_mode_headless_stream_copy'),
    impact: $t('config.av_mode_headless_stream_impact'),
    technical: $t('config.av_mode_headless_stream_technical'),
  },
  {
    id: 'windowed_stream',
    title: $t('config.av_mode_windowed_stream_title'),
    badge: $t('config.av_mode_windowed_stream_badge'),
    group: 'private',
    copy: $t('config.av_mode_windowed_stream_copy'),
    impact: $t('config.av_mode_windowed_stream_impact'),
    technical: $t('config.av_mode_windowed_stream_technical'),
  },
  {
    id: 'gamescope_stream',
    title: $t('config.av_mode_gamescope_stream_title'),
    badge: $t('config.av_mode_gamescope_stream_badge'),
    group: 'private',
    copy: $t('config.av_mode_gamescope_stream_copy'),
    impact: $t('config.av_mode_gamescope_stream_impact'),
    technical: $t('config.av_mode_gamescope_stream_technical'),
  },
  {
    id: 'host_virtual_display',
    title: $t('config.av_mode_host_virtual_display_title'),
    badge: $t('config.av_mode_host_virtual_display_badge'),
    group: 'host',
    copy: $t('config.av_mode_host_virtual_display_copy'),
    impact: $t('config.av_mode_host_virtual_display_impact'),
    technical: $t('config.av_mode_host_virtual_display_technical'),
  },
  {
    id: 'headless_dongle',
    title: $t('config.av_mode_headless_dongle_title'),
    badge: $t('config.av_mode_headless_dongle_badge'),
    group: 'host',
    copy: $t('config.av_mode_headless_dongle_copy'),
    impact: $t('config.av_mode_headless_dongle_impact'),
    technical: $t('config.av_mode_headless_dongle_technical'),
  },
  {
    id: 'desktop_display',
    title: $t('config.av_mode_desktop_display_title'),
    badge: $t('config.av_mode_desktop_display_badge'),
    group: 'host',
    copy: $t('config.av_mode_desktop_display_copy'),
    impact: $t('config.av_mode_desktop_display_impact'),
    technical: $t('config.av_mode_desktop_display_technical'),
  },
]

const streamDisplayModes = computed(() => streamDisplayModeDefinitions.map((mode) => {
  const hostMode = projectionModes.value?.find((candidate) => candidate?.value === mode.id)
  const availability = hostMode
    ? {
        available: hostMode.available === true,
        unavailableReason: hostMode.available === true
          ? ''
          : String(hostMode.unavailable_reason || $t('config.av_mode_unavailable_default')),
      }
    : resolveStreamDisplayModeAvailability(mode.id, config.value.stream_display_mode_options)
  const hostBadge = hostMode && typeof hostMode.badge === 'string' ? hostMode.badge.trim() : ''
  return {
    ...mode,
    ...availability,
    badge: hostBadge || mode.badge,
    hostBadge: Boolean(hostBadge),
  }
}))

const plannedStreamDisplayModes = [
  {
    title: $t('config.av_planned_family_mode_title'),
    copy: $t('config.av_planned_family_mode_copy'),
  },
  {
    title: $t('config.av_planned_headless_evdi_title'),
    copy: $t('config.av_planned_headless_evdi_copy'),
  },
]

const streamDisplayMode = computed(() => resolveStreamDisplayMode(config.value))

const selectedStreamDisplayMode = computed(() => (
  streamDisplayModes.value.find((mode) => mode.id === streamDisplayMode.value) || streamDisplayModes.value[0]
))

const clientSettingsSync = computed(() => resolveClientSettingsSync(config.value))
const streamDisplayRuntimeNotice = computed(() => (
  resolveStreamDisplayRuntimeNotice(clientSettingsSync.value, streamDisplayMode.value)
))
const streamDisplayRuntimeNoticeTone = computed(() => {
  if (streamDisplayRuntimeNotice.value.state === 'synced') {
    return 'border-success/25 bg-success/10 text-success-bright'
  }
  if (streamDisplayRuntimeNotice.value.state === 'pending_relaunch' || streamDisplayRuntimeNotice.value.state === 'unsaved') {
    return 'border-warning/25 bg-warning/10 text-warning-bright'
  }
  return 'border-storm/30 bg-deep/40 text-storm'
})
const clientSettingsSyncBadge = computed(() => {
  if (!clientSettingsSync.value.available) return $t('config.av_nova_sync_badge_unavailable')
  if (clientSettingsSync.value.relaunchRequired) return $t('config.av_nova_sync_badge_pending_relaunch')
  return $t('config.av_nova_sync_badge_bidirectional')
})
const clientSettingsSyncTone = computed(() => {
  if (!clientSettingsSync.value.available || clientSettingsSync.value.relaunchRequired) {
    return 'border-warning/30 bg-warning/10 text-warning-bright'
  }
  return 'border-success/30 bg-success/10 text-success'
})
const clientSettingsSyncCopy = computed(() => {
  if (!clientSettingsSync.value.available) {
    return $t('config.av_nova_sync_copy_unavailable')
  }
  if (clientSettingsSync.value.relaunchRequired) {
    return $t('config.av_nova_sync_copy_pending_relaunch')
  }
  return $t('config.av_nova_sync_copy_synced')
})
const streamDisplayRelaunchRequired = computed(() => (
  hostStreamDisplay.value
    ? hostStreamDisplay.value.relaunch_required === true
    : clientSettingsSync.value.relaunchRequired
))
// The newest committed config write, from either writer, when the host reports it.
const lastConfigWrite = computed(() => {
  const notes = projection.provenance.value
  return Array.isArray(notes) && notes.length > 0 && notes[0] && typeof notes[0] === 'object' ? notes[0] : null
})
const lastConfigWriteRow = computed(() => {
  const note = lastConfigWrite.value
  if (!note) return null
  const writerKey = note.writer === 'gamestream' ? 'gamestream' : note.writer === 'web_ui' ? 'web_ui' : 'other'
  const keys = Array.isArray(note.keys) ? note.keys : []
  return {
    label: $t('config.av_nova_sync_row_last_write'),
    value: $t(`config.av_provenance_writer_${writerKey}`),
    note: $t('config.av_provenance_keys_note', { count: keys.length, at: String(note.at || '') }),
  }
})
const clientSettingsRows = computed(() => [
  {
    label: $t('config.av_nova_sync_row_display_mode'),
    value: hostStreamDisplay.value?.configured_label || clientSettingsSync.value.desiredModeLabel,
    note: $t('config.av_nova_sync_note_next_stream'),
  },
  {
    label: $t('config.av_nova_sync_row_effective_mode'),
    value: hostStreamDisplay.value?.effective_label || clientSettingsSync.value.effectiveModeLabel,
    note: streamDisplayRelaunchRequired.value
      ? $t('config.av_nova_sync_note_pending')
      : $t('config.av_nova_sync_note_synced'),
  },
  ...(lastConfigWriteRow.value ? [lastConfigWriteRow.value] : []),
])

const autoQualityEnabled = computed(() => (
  config.value.ai_enabled === 'enabled' && config.value.adaptive_bitrate_enabled === 'enabled'
))
// The split state is reachable when the config file was edited by hand or an
// older host upgraded: exactly one of the pair is on.
const autoQualityPartial = computed(() => (
  (config.value.ai_enabled === 'enabled') !== (config.value.adaptive_bitrate_enabled === 'enabled')
))
// Live strip: present when the host serves the projection and a policy snapshot.
const autoQualityLive = computed(() => Boolean(projection.ok.value && liveAutoQuality.value))
const autoQualityLiveStateKey = computed(() => autoQualityHostStateKey(liveAutoQuality.value))
const autoQualityLiveRows = computed(() => buildLiveAutoQualityRows(
  { autoQuality: liveAutoQuality.value, tuning: liveTuning.value },
  $t,
))
const autoQualityBadge = computed(() => {
  if (autoQualityLive.value) {
    return $t('config.av_auto_quality_badge_live', {
      state: $t(`config.av_auto_quality_live_state_${autoQualityLiveStateKey.value}`),
    })
  }
  if (autoQualityEnabled.value) return $t('config.av_auto_quality_badge_on')
  if (autoQualityPartial.value) return $t('config.av_auto_quality_badge_partial')
  return $t('config.av_auto_quality_badge_manual')
})
const autoQualityTone = computed(() => {
  if (autoQualityLive.value) {
    const tone = autoQualityHostTone(autoQualityLiveStateKey.value)
    if (tone === 'pass') return 'border-success/30 bg-success/10 text-success'
    if (tone === 'warning') return 'border-warning/30 bg-warning/10 text-warning-bright'
    return 'border-storm/40 bg-storm/10 text-storm'
  }
  if (autoQualityEnabled.value) return 'border-success/30 bg-success/10 text-success'
  if (autoQualityPartial.value) return 'border-warning/30 bg-warning/10 text-warning-bright'
  return 'border-storm/40 bg-storm/10 text-storm'
})
const autoQualityCopy = computed(() => {
  if (autoQualityEnabled.value) {
    return $t('config.av_auto_quality_copy_on')
  }
  if (autoQualityPartial.value) {
    return $t('config.av_auto_quality_copy_partial')
  }
  return $t('config.av_auto_quality_copy_manual')
})
const autoQualityRows = computed(() => [
  {
    label: $t('config.av_auto_quality_row_profile'),
    value: config.value.ai_enabled === 'enabled'
      ? $t('config.av_auto_quality_profile_auto')
      : $t('config.av_auto_quality_profile_manual'),
    note: config.value.ai_enabled === 'enabled'
      ? $t('config.av_auto_quality_profile_note_auto')
      : $t('config.av_auto_quality_profile_note_manual'),
  },
  {
    label: $t('config.av_auto_quality_row_bitrate'),
    value: config.value.adaptive_bitrate_enabled === 'enabled'
      ? $t('config.av_auto_quality_bitrate_adaptive')
      : $t('config.av_auto_quality_bitrate_fixed'),
    note: config.value.adaptive_bitrate_enabled === 'enabled'
      ? $t('config.av_auto_quality_bitrate_range_note', {
          min: Number(config.value.adaptive_bitrate_min || 0) / 1000,
          max: Number(config.value.adaptive_bitrate_max || 0) / 1000,
        })
      : $t('config.av_auto_quality_bitrate_cap_note', { cap: Number(config.value.max_bitrate || 0) / 1000 }),
  },
  {
    label: $t('config.av_auto_quality_row_runtime'),
    value: selectedStreamDisplayMode.value.title,
    note: selectedStreamDisplayMode.value.badge,
  },
  {
    label: $t('config.av_auto_quality_row_nova'),
    value: clientSettingsSyncBadge.value,
    note: clientSettingsSync.value.relaunchRequired
      ? $t('config.av_auto_quality_nova_note_relaunch')
      : $t('config.av_auto_quality_nova_note_ready'),
  },
])
const isLabwcPath = computed(() => (
  streamDisplayMode.value === 'headless_stream' || streamDisplayMode.value === 'windowed_stream'
))
const isGamescopePath = computed(() => streamDisplayMode.value === 'gamescope_stream')
const isDonglePath = computed(() => streamDisplayMode.value === 'headless_dongle')

const nvidiaTrueHeadlessGpuNativeGuard = computed(() => (
  isLabwcPath.value &&
  String(config.value.encoder || '').toLowerCase() === 'nvenc' &&
  config.value.headless_mode === 'enabled' &&
  config.value.linux_use_cage_compositor === 'enabled' &&
  config.value.linux_prefer_gpu_native_capture !== 'enabled'
))
const linuxStreamingSetupChecklist = computed(() => {
  const items = [
    {
      id: 'path',
      title: $t('config.av_checklist_path_title'),
      status: selectedStreamDisplayMode.value.title,
      copy: isGamescopePath.value
        ? $t('config.av_checklist_path_copy_gamescope')
        : isDonglePath.value
          ? $t('config.av_checklist_path_copy_dongle')
          : isLabwcPath.value
            ? $t('config.av_checklist_path_copy_labwc')
            : $t('config.av_checklist_path_copy_mirror'),
    },
    {
      id: 'encoder',
      title: $t('config.av_checklist_encoder_title'),
      status: autoQualityBadge.value,
      copy: autoQualityEnabled.value
        ? $t('config.av_checklist_encoder_copy_auto')
        : $t('config.av_checklist_encoder_copy_manual'),
    },
  ]
  if (isLabwcPath.value) {
    items.push({
      id: 'wayland-vaapi',
      title: $t('config.av_checklist_gpu_native_title'),
      status: config.value.linux_prefer_gpu_native_capture === 'enabled'
        ? $t('config.av_checklist_gpu_native_status_requested')
        : $t('config.av_checklist_gpu_native_status_default'),
      copy: config.value.linux_prefer_gpu_native_capture === 'enabled'
        ? $t('config.av_checklist_gpu_native_copy_requested')
        : $t('config.av_checklist_gpu_native_copy_default'),
    })
  }
  if (isGamescopePath.value) {
    items.push({
      id: 'gamescope-host',
      title: $t('config.av_checklist_gamescope_title'),
      status: $t('config.av_checklist_gamescope_status'),
      copy: $t('config.av_checklist_gamescope_copy'),
    })
  }
  if (nvidiaTrueHeadlessGpuNativeGuard.value) {
    items.push({
      id: 'nvidia-headless-gpu-native-guard',
      title: $t('config.av_checklist_nvidia_guard_title'),
      status: $t('config.av_checklist_nvidia_guard_status'),
      copy: $t('config.av_checklist_nvidia_guard_copy'),
    })
  }
  return items
})

function setEnabledConfig(key, enabled) {
  config.value[key] = enabled ? 'enabled' : 'disabled'
}

function setAutoQuality(enabled) {
  setEnabledConfig('adaptive_bitrate_enabled', enabled)
  setEnabledConfig('ai_enabled', enabled)
}

const dongleOutputs = ref([])
const dongleDetectStatus = ref('')
let dongleRequestGeneration = 0

async function refreshDongleOutputs() {
  const requestGeneration = ++dongleRequestGeneration
  dongleDetectStatus.value = 'Detecting…'
  try {
    const res = await fetch('/api/linux/display-outputs', { credentials: 'include' })
    const data = await res.json()
    if (requestGeneration !== dongleRequestGeneration || streamDisplayMode.value !== 'headless_dongle') {
      return
    }
    if (!data?.status) {
      dongleDetectStatus.value = 'Detection failed'
      return
    }
    dongleOutputs.value = data.outputs || []
    // Auto-fill empty fields from host discovery (sysfs DRM).
    if (!config.value.linux_streaming_output && data.suggested_streaming_output) {
      config.value.linux_streaming_output = data.suggested_streaming_output
    }
    if (!config.value.linux_primary_output && data.suggested_primary_output) {
      config.value.linux_primary_output = data.suggested_primary_output
    }
    if (!config.value.headless_swap_mode) {
      config.value.headless_swap_mode = 'privacy'
    }
    config.value.linux_auto_manage_displays = 'enabled'
    const n = dongleOutputs.value.filter((o) => o.connected).length
    dongleDetectStatus.value = n
      ? `Found ${n} connected connector(s); suggestions applied if fields were empty`
      : 'No connected connectors reported (plug dongle / check DRM)'
  } catch (e) {
    if (requestGeneration === dongleRequestGeneration && streamDisplayMode.value === 'headless_dongle') {
      dongleDetectStatus.value = 'Detection request failed'
    }
  }
}

function setStreamDisplayMode(mode) {
  if (streamDisplayModes.value.find((candidate) => candidate.id === mode)?.available !== true) {
    return
  }
  if (mode !== 'headless_dongle') {
    ++dongleRequestGeneration
  }
  const next = applyStreamDisplayModeToConfig(config.value, mode)
  config.value.headless_mode = next.headless_mode
  config.value.linux_use_cage_compositor = next.linux_use_cage_compositor
  config.value.linux_prefer_gpu_native_capture = next.linux_prefer_gpu_native_capture
  config.value.linux_stream_mode = next.linux_stream_mode
  config.value.linux_private_runtime = next.linux_private_runtime
  config.value.linux_auto_manage_displays = next.linux_auto_manage_displays
  config.value.headless_swap_mode = next.headless_swap_mode
  config.value.capture = next.capture
}

watch(streamDisplayMode, (mode) => {
  if (mode === 'headless_dongle' && dongleOutputs.value.length === 0) {
    refreshDongleOutputs()
  }
})

onMounted(() => {
  if (streamDisplayMode.value === 'headless_dongle') {
    refreshDongleOutputs()
  }
})

function applyDisplayPlan(choice) {
  if (!choice?.safe || config.value.fallback_mode === choice.targetMode) return
  applyingDisplayPlan = true
  config.value.fallback_mode = choice.targetMode
  // Custom scales are not reconstructable after reload, so only preset ids persist.
  config.value.display_plan = PERSISTABLE_PLAN_IDS.includes(choice.id) ? choice.id : ''
}

const validateFallbackMode = (event) => {
  const value = event.target.value;
  if (!value.match(/^\d+x\d+x\d+(\.\d+)?$/)) {
    event.target.setCustomValidity($t('config.fallback_mode_error'));
  } else {
    event.target.setCustomValidity('');
  }

  event.target.reportValidity();
}

function updateDisplayPlannerSource(event) {
  validateFallbackMode(event)
  plannerSourceMode.value = event.target.value
}
</script>

<template>
  <div id="audio-video" class="config-page">
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Streaming</div>
        <h3 class="settings-section-title">Where games run</h3>
      </div>

      <div class="settings-inline-stack">
        <div v-if="isLinux" class="settings-subtle-surface space-y-4">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0">
              <div class="section-kicker">Launch mode</div>
              <div class="mt-2 text-sm text-storm">
                Choose what appears on the stream and whether the host monitors are used. Polaris chooses the capture method automatically.
                <a href="https://papi-ux.com/docs/launch-modes/" target="_blank" class="focus-ring text-ice hover:underline">Launch mode guide</a>
              </div>
            </div>
            <span class="shrink-0 whitespace-nowrap rounded-full border border-ice/20 bg-ice/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-ice">
              {{ selectedStreamDisplayMode.title }}
            </span>
          </div>

          <div class="grid gap-3 xl:grid-cols-2" data-stream-display-mode-picker>
            <article
              v-for="mode in streamDisplayModes"
              :key="mode.id"
              class="overflow-hidden rounded-lg border transition"
              :class="[
                streamDisplayMode === mode.id ? 'border-ice bg-ice/12 shadow-[0_0_0_1px_rgba(224,230,237,0.18)]' : 'border-storm/40 bg-deep/40',
                mode.available === false
                  ? 'opacity-60'
                  : 'hover:border-storm/70',
              ]"
            >
              <SelectableCard
                card-class="min-h-[132px] w-full p-4"
                :disabled="mode.available === false"
                :selected="streamDisplayMode === mode.id"
                @click="setStreamDisplayMode(mode.id)"
              >
                <span class="flex items-start justify-between gap-3">
                  <span class="flex min-w-0 items-center gap-2.5">
                    <span
                      class="inline-flex h-5 w-5 shrink-0 items-center justify-center rounded-full border"
                      :class="streamDisplayMode === mode.id ? 'border-ice bg-ice text-void' : 'border-storm/50 bg-void/30 text-transparent'"
                      aria-hidden="true"
                    >
                      <svg class="h-3 w-3" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="3" d="m5 12 4 4L19 6" /></svg>
                    </span>
                    <span class="text-sm font-semibold text-silver">{{ mode.title }}</span>
                  </span>
                  <span
                    class="shrink-0 rounded-full border px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow"
                    :class="mode.id === 'headless_stream'
                      ? 'border-success/30 bg-success/10 text-success'
                      : mode.id === 'windowed_stream'
                        ? 'border-warning/40 bg-warning/10 text-warning-bright'
                        : mode.id === 'gamescope_stream'
                          ? 'border-ice/30 bg-ice/10 text-ice'
                          : 'border-storm/40 bg-storm/10 text-storm'"
                  >
                    {{ mode.badge }}
                  </span>
                </span>
                <span class="mt-3 block text-sm leading-relaxed text-silver">{{ mode.copy }}</span>
                <span class="mt-2 block text-xs leading-relaxed text-storm">{{ mode.impact }}</span>
                <span
                  v-if="mode.available === false"
                  class="mt-2 block text-xs font-medium leading-relaxed text-warning-bright"
                >
                  {{ $t('config.av_mode_unavailable', { reason: mode.unavailableReason }) }}
                </span>
              </SelectableCard>
            </article>
          </div>

          <div class="rounded-xl border border-ice/25 bg-ice/5 p-4" data-selected-stream-path>
            <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
              <div class="min-w-0">
                <div class="section-kicker">Current path</div>
                <h4 class="mt-1 text-base font-semibold text-silver">{{ selectedStreamDisplayMode.title }}</h4>
                <p class="mt-2 text-sm leading-relaxed text-storm">{{ selectedStreamDisplayMode.copy }}</p>
              </div>
              <span class="meta-pill shrink-0 border-ice/25 bg-ice/10 text-ice">Selected</span>
            </div>
            <div class="mt-3 grid gap-3 lg:grid-cols-2">
              <StatTile tile-class="py-2.5" label="Player impact">
                <template #value>
                  <div class="mt-1 text-xs leading-relaxed text-storm">{{ selectedStreamDisplayMode.impact }}</div>
                </template>
              </StatTile>
              <StatTile tile-class="py-2.5" label="Capture path">
                <template #value>
                  <div class="mt-1 text-xs leading-relaxed text-storm">{{ selectedStreamDisplayMode.technical }}</div>
                </template>
              </StatTile>
            </div>
          </div>

          <div
            class="rounded-2xl border p-4 text-sm leading-relaxed"
            :class="streamDisplayRuntimeNoticeTone"
            data-stream-display-runtime-notice
          >
            {{ streamDisplayRuntimeNotice.copy }}
          </div>

          <div
            v-if="streamDisplayMode === 'headless_dongle'"
            class="settings-subtle-surface space-y-3"
            data-dongle-outputs
          >
            <div class="flex flex-wrap items-center justify-between gap-2">
              <div class="section-kicker">Dongle outputs</div>
              <button
                type="button"
                class="focus-ring rounded-lg border border-storm/40 px-2 py-1 text-xs text-silver hover:border-ice"
                @click="refreshDongleOutputs"
              >
                Detect connectors
              </button>
            </div>
            <p class="text-sm text-storm">
              Auto-detect uses DRM sysfs (fast). Pick the dummy plug as streaming and the real panel as primary, then save.
            </p>
            <p v-if="dongleDetectStatus" class="text-xs text-ice">{{ dongleDetectStatus }}</p>
            <div class="grid gap-3 sm:grid-cols-2">
              <label class="block text-sm text-storm">
                Streaming output (dongle)
                <select
                  v-model="config.linux_streaming_output"
                  class="settings-input mt-1"
                >
                  <option value="">— select —</option>
                  <option
                    v-for="o in dongleOutputs"
                    :key="'s-' + o.name"
                    :value="o.name"
                  >
                    {{ o.name }}{{ o.connected ? ' (connected)' : '' }}{{ o.likely_dongle ? ' · dongle?' : '' }}{{ o.suggested_streaming ? ' · suggested' : '' }}
                  </option>
                </select>
                <input
                  v-model="config.linux_streaming_output"
                  type="text"
                  class="mt-2 w-full rounded-lg border border-storm/40 bg-void/40 px-3 py-1.5 font-mono text-xs text-silver"
                  placeholder="or type e.g. HDMI-A-2"
                />
              </label>
              <label class="block text-sm text-storm">
                Primary output (real panel)
                <select
                  v-model="config.linux_primary_output"
                  class="settings-input mt-1"
                >
                  <option value="">— select —</option>
                  <option
                    v-for="o in dongleOutputs"
                    :key="'p-' + o.name"
                    :value="o.name"
                  >
                    {{ o.name }}{{ o.connected ? ' (connected)' : '' }}{{ o.enabled ? ' · enabled' : '' }}{{ o.suggested_primary ? ' · suggested' : '' }}
                  </option>
                </select>
                <input
                  v-model="config.linux_primary_output"
                  type="text"
                  class="mt-2 w-full rounded-lg border border-storm/40 bg-void/40 px-3 py-1.5 font-mono text-xs text-silver"
                  placeholder="or type e.g. DP-3"
                />
              </label>
              <label class="block text-sm text-storm sm:col-span-2">
                Swap mode
                <select
                  v-model="config.headless_swap_mode"
                  class="settings-input mt-1"
                >
                  <option value="privacy">privacy — blank panel after portal approval</option>
                  <option value="off">off — extended, panel stays primary</option>
                </select>
              </label>
            </div>
          </div>

          <div
            v-if="isGamescopePath"
            class="rounded-lg border border-ice/20 bg-ice/5 px-3 py-2 text-sm text-storm"
          >
            <strong class="text-silver">Gamescope Stream</strong> attaches to
            <code class="text-ice">gamescope-0</code> (idle unit) or spawns owned headless Gamescope.
            Capture is portal/PipeWire. Settings that matter: path card, encoder, bitrate/HDR, apps.
            labwc-only flags (cage compositor, GPU-native preference) are ignored on this path.
            Save and restart Polaris after switching.
          </div>

          <details class="settings-disclosure rounded-xl border border-storm/30 bg-deep/25" data-linux-path-details>
            <summary class="settings-disclosure-summary p-4">
              <div>
                <div class="section-kicker">Advanced & diagnostics</div>
                <h4 class="mt-1 text-sm font-semibold text-silver">Setup, client sync, and technical controls</h4>
                <div class="mt-1 text-xs leading-relaxed text-storm">Open this when configuring a new path or investigating a stream that does not match the selected mode.</div>
              </div>
              <svg class="settings-disclosure-chevron h-4 w-4 shrink-0 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
            </summary>

            <div class="space-y-3 border-t border-storm/20 p-4">
              <StatTile tile-class="p-3" data-capture-path-explainer>
                <div class="text-sm font-semibold text-silver">How capture works</div>
                <p class="mt-1 text-xs leading-relaxed text-storm">
                  Polaris picks the capture path after the launch mode is set: GPU-native keeps frames on the GPU, while System-memory capture copies through RAM and can be the intended safe path on AMD and Intel.
                  <a href="https://papi-ux.com/docs/launch-modes/#how-capture-works" target="_blank" class="focus-ring text-ice hover:underline">How capture works</a>
                </p>
              </StatTile>

              <div class="settings-subtle-surface" data-linux-streaming-setup>
                <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
                  <div>
                    <div class="section-kicker">Linux Streaming Setup</div>
                    <h4 class="mt-2 text-sm font-semibold text-silver">Minimal checklist for this path</h4>
                    <div class="mt-1 text-sm leading-relaxed text-storm">
                      Default is <strong class="text-silver">Private Stream (labwc)</strong>. Gamescope works when the host stack is ready.
                      Only the steps for the selected path are shown.
                    </div>
                  </div>
                  <span class="meta-pill shrink-0">{{ selectedStreamDisplayMode.title }}</span>
                </div>

                <div class="mt-4 grid gap-3 xl:grid-cols-2">
                  <StatTile v-for="item in linuxStreamingSetupChecklist" :key="item.id" tile-class="py-3">
                    <div class="flex items-start justify-between gap-3">
                      <div class="text-sm font-semibold text-silver">{{ item.title }}</div>
                      <span class="rounded-full border border-storm/30 bg-storm/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-storm">{{ item.status }}</span>
                    </div>
                    <div class="mt-2 text-sm leading-relaxed text-storm">{{ item.copy }}</div>
                  </StatTile>
                </div>
                <div class="mt-3 text-xs text-storm">
                  Full setup detail: <a href="https://papi-ux.com/docs/launch-modes/#linux-setup-checklist" target="_blank" class="focus-ring text-ice hover:underline">Linux setup checklist</a>
                </div>
              </div>

              <div class="settings-subtle-surface">
                <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
                  <div>
                    <div class="section-kicker">Nova Sync</div>
                    <div class="mt-2 text-sm leading-relaxed text-storm">{{ clientSettingsSyncCopy }}</div>
                  </div>
                  <span class="meta-pill shrink-0" :class="clientSettingsSyncTone">{{ clientSettingsSyncBadge }}</span>
                </div>
                <div class="mt-4 grid gap-2 sm:grid-cols-2">
                  <StatTile v-for="row in clientSettingsRows" :key="row.label" :label="row.label" :value="row.value" :note="row.note" />
                </div>
              </div>

              <div class="grid gap-3 xl:grid-cols-3">
                <div class="surface-muted p-3">
                  <div class="text-xs font-semibold uppercase tracking-eyebrow text-accent">Isolation</div>
                  <div class="mt-2 text-sm leading-relaxed text-storm">
                    {{ isGamescopePath
                      ? 'Gamescope Stream isolates paint in gamescope-0 (portal capture).'
                      : 'Private Stream (labwc) keeps apps off your real desktop — the default stability target.' }}
                  </div>
                </div>
                <div class="surface-muted p-3">
                  <div class="text-xs font-semibold uppercase tracking-eyebrow text-success">GPU path</div>
                  <div class="mt-2 text-sm leading-relaxed text-storm">
                    {{ isLabwcPath
                      ? 'GPU-native is a labwc capture preference. Force it only when diagnostics show CPU/SHM fallback.'
                      : isGamescopePath
                        ? 'Gamescope uses portal/PipeWire capture; labwc GPU-native flags do not apply.'
                        : 'Capture backend follows the path (portal for mirror/dongle; KMS only if you set capture=kms).' }}
                  </div>
                </div>
                <div class="surface-muted p-3">
                  <div class="text-xs font-semibold uppercase tracking-eyebrow text-warning-bright">FPS target</div>
                  <div class="mt-2 text-sm leading-relaxed text-storm">A 120 FPS client target still needs the game/output to render above 60 FPS; Polaris will show the live gap on the dashboard.</div>
                </div>
              </div>

              <details v-if="isLabwcPath" class="settings-disclosure rounded-lg border border-storm/30 bg-deep/30">
                <summary class="settings-disclosure-summary p-4">
                  <div>
                    <div class="section-kicker">Advanced</div>
                    <h4 class="mt-2 text-sm font-semibold text-silver">labwc runtime flags</h4>
                    <div class="mt-1 text-sm text-storm">These keys only affect Private Stream / labwc paths. Hidden when Gamescope or host paths are selected.</div>
                  </div>
                  <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
                </summary>

                <div class="grid gap-4 p-4 pt-0 xl:grid-cols-2">
                  <div class="surface-muted p-4">
                    <div class="text-sm font-medium text-silver">Hidden-output request</div>
                    <div class="mt-1 text-sm text-storm">Existing headless_mode config key. Enabled by Private Stream and Host Virtual Display.</div>
                    <div class="mt-3 rounded bg-deep/60 px-2 py-1 font-mono text-xs text-storm">headless_mode</div>
                    <label class="mt-4 flex items-center justify-between gap-4">
                      <span class="text-xs uppercase tracking-eyebrow text-storm">Requested</span>
                      <input
                        type="checkbox"
                        class="sr-only peer"
                        :checked="config.headless_mode === 'enabled'"
                        @change="config.headless_mode = $event.target.checked ? 'enabled' : 'disabled'"
                      >
                      <div class="relative h-5 w-9 rounded-full bg-storm/40 transition-colors peer-checked:bg-accent after:absolute after:left-[2px] after:top-[2px] after:h-4 after:w-4 after:rounded-full after:bg-white after:transition-all after:content-[''] peer-checked:after:translate-x-full"></div>
                    </label>
                  </div>

                  <div class="surface-muted p-4">
                    <div class="text-sm font-medium text-silver">Private compositor runtime</div>
                    <div class="mt-1 text-sm text-storm">Existing linux_use_cage_compositor config key. Enabled by Private Stream and Private Stream (GPU-native).</div>
                    <div class="mt-3 rounded bg-deep/60 px-2 py-1 font-mono text-xs text-storm">linux_use_cage_compositor</div>
                    <label class="mt-4 flex items-center justify-between gap-4">
                      <span class="text-xs uppercase tracking-eyebrow text-storm">Use labwc</span>
                      <input
                        type="checkbox"
                        class="sr-only peer"
                        :checked="config.linux_use_cage_compositor === 'enabled'"
                        @change="config.linux_use_cage_compositor = $event.target.checked ? 'enabled' : 'disabled'"
                      >
                      <div class="relative h-5 w-9 rounded-full bg-storm/40 transition-colors peer-checked:bg-accent after:absolute after:left-[2px] after:top-[2px] after:h-4 after:w-4 after:rounded-full after:bg-white after:transition-all after:content-[''] peer-checked:after:translate-x-full"></div>
                    </label>
                  </div>

                  <div class="surface-muted p-4">
                    <div class="text-sm font-medium text-silver">GPU-native capture preference</div>
                    <div class="mt-1 text-sm text-storm">Existing linux_prefer_gpu_native_capture config key. Enabled by Private Stream (GPU-native). When active, Polaris may force labwc windowed instead of hidden headless so capture can stay on the GPU if Private Stream would otherwise fall back.</div>
                    <div class="mt-3 rounded bg-deep/60 px-2 py-1 font-mono text-xs text-storm">linux_prefer_gpu_native_capture</div>
                    <label class="mt-4 flex items-center justify-between gap-4">
                      <span class="text-xs uppercase tracking-eyebrow text-storm">Performance</span>
                      <input
                        type="checkbox"
                        class="sr-only peer"
                        :checked="config.linux_prefer_gpu_native_capture === 'enabled'"
                        @change="config.linux_prefer_gpu_native_capture = $event.target.checked ? 'enabled' : 'disabled'"
                      >
                      <div class="relative h-5 w-9 rounded-full bg-storm/40 transition-colors peer-checked:bg-accent after:absolute after:left-[2px] after:top-[2px] after:h-4 after:w-4 after:rounded-full after:bg-white after:transition-all after:content-[''] peer-checked:after:translate-x-full"></div>
                    </label>
                  </div>

                  <div class="surface-muted p-4">
                    <div class="text-sm font-medium text-silver">Capture telemetry profiling</div>
                    <div class="mt-1 text-sm text-storm">Emit timing summaries while validating capture backends.</div>
                    <div class="mt-3 rounded bg-deep/60 px-2 py-1 font-mono text-xs text-storm">linux_capture_profile</div>
                    <label class="mt-4 flex items-center justify-between gap-4">
                      <span class="text-xs uppercase tracking-eyebrow text-storm">Diagnostics</span>
                      <input
                        type="checkbox"
                        class="sr-only peer"
                        :checked="config.linux_capture_profile === 'enabled'"
                        @change="config.linux_capture_profile = $event.target.checked ? 'enabled' : 'disabled'"
                      >
                      <div class="relative h-5 w-9 rounded-full bg-storm/40 transition-colors peer-checked:bg-accent after:absolute after:left-[2px] after:top-[2px] after:h-4 after:w-4 after:rounded-full after:bg-white after:transition-all after:content-[''] peer-checked:after:translate-x-full"></div>
                    </label>
                  </div>
                </div>
              </details>

              <details class="rounded-lg border border-storm/30 bg-deep/30 p-4" data-planned-stream-modes>
                <summary class="focus-ring cursor-pointer select-none text-sm font-semibold text-silver">Planned modes</summary>
                <div class="mt-3 grid gap-3 sm:grid-cols-2">
                  <div v-for="mode in plannedStreamDisplayModes" :key="mode.title" class="rounded-lg border border-storm/20 bg-void/20 p-3">
                    <div class="text-sm font-medium text-silver">{{ mode.title }}</div>
                    <div class="mt-1 text-xs leading-relaxed text-storm">{{ mode.copy }}</div>
                  </div>
                </div>
              </details>
            </div>
          </details>
        </div>

        <div v-if="isWindows" class="settings-subtle-surface">
          <div class="section-kicker">Windows display strategy</div>
          <div class="mt-2 grid gap-3 xl:grid-cols-2">
            <Checkbox
              id="headless_mode"
              locale-prefix="config"
              v-model="config.headless_mode"
              default="false"
            ></Checkbox>

            <Checkbox
              id="double_refreshrate"
              locale-prefix="config"
              v-model="config.double_refreshrate"
              default="false"
            ></Checkbox>

            <Checkbox
              id="isolated_virtual_display_option"
              locale-prefix="config"
              v-model="config.isolated_virtual_display_option"
              default="false"
            ></Checkbox>
          </div>
        </div>

        <AdapterNameSelector
          :platform="platform"
          :config="config"
        />
      </div>
    </section>

    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Auto Quality</div>
        <h3 class="settings-section-title">Performance and quality balance</h3>
        <div class="settings-summary-copy">One primary mode for bitrate, launch profile, and recovery behavior. Advanced controls stay available below.</div>
      </div>

      <div class="settings-subtle-surface space-y-4">
        <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
          <div class="min-w-0">
            <div class="flex flex-wrap items-center gap-2">
              <div class="text-base font-semibold text-silver">
                {{ autoQualityEnabled ? $t('config.av_auto_quality_heading_on') : autoQualityPartial ? $t('config.av_auto_quality_heading_partial') : $t('config.av_auto_quality_heading_manual') }}
              </div>
              <span class="meta-pill" :class="autoQualityTone">{{ autoQualityBadge }}</span>
            </div>
            <div class="mt-2 max-w-3xl text-sm leading-relaxed text-storm">{{ autoQualityCopy }}</div>
          </div>
          <button
            type="button"
            class="focus-ring dashboard-action-button"
            :class="autoQualityEnabled ? 'dashboard-action-button-secondary' : 'dashboard-action-button-primary'"
            @click="setAutoQuality(!autoQualityEnabled)"
          >
            {{ autoQualityEnabled ? $t('config.av_auto_quality_disable_action') : $t('config.av_auto_quality_enable_action') }}
          </button>
        </div>

        <div class="flex flex-wrap items-center justify-between gap-2">
          <div class="section-kicker" data-auto-quality-strip-source="{{ autoQualityLive ? 'host' : 'saved' }}">
            {{ autoQualityLive ? $t('config.av_auto_quality_live_kicker') : $t('config.av_auto_quality_saved_kicker') }}
          </div>
          <p
            v-if="provenanceFor('adaptive_bitrate_enabled')"
            class="text-[11px] text-storm"
            data-provenance="adaptive_bitrate_enabled"
          >
            {{ $t('config.av_provenance_set_by', { source: provenanceFor('adaptive_bitrate_enabled') }) }}
          </p>
        </div>
        <div class="grid gap-2 sm:grid-cols-2 xl:grid-cols-4" data-auto-quality-strip>
          <StatTile
            v-for="row in (autoQualityLive ? autoQualityLiveRows : autoQualityRows)"
            :key="row.label"
            :label="row.label"
            :value="row.value"
            :note="row.note"
          />
        </div>
      </div>
    </section>

    <details class="settings-section settings-disclosure" open>
      <summary class="settings-disclosure-summary">
        <div>
          <div class="section-kicker">Audio</div>
          <h3 class="settings-section-title mt-2">Host audio capture</h3>
          <div class="settings-summary-copy">Choose the sink Polaris captures from and keep the host audio path predictable.</div>
        </div>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
      </summary>

      <div class="settings-disclosure-body settings-inline-stack">
        <Checkbox
          id="stream_audio"
          locale-prefix="config"
          v-model="config.stream_audio"
          default="true"
        ></Checkbox>

        <div class="mb-3">
          <label for="audio_sink" class="block text-sm font-medium text-storm mb-1">{{ $t('config.audio_sink') }}</label>
          <input
            id="audio_sink"
            v-model="config.audio_sink"
            type="text"
            class="settings-input"
            :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
          />
          <div class="text-sm text-storm mt-1">{{ $tp('config.audio_sink_desc') }}</div>
          <div class="settings-subtle-surface mt-3 text-sm text-storm">
            <div class="section-kicker">Lookup commands</div>
            <PlatformLayout :platform="platform">
              <template #windows>
                <pre class="mt-2 overflow-x-auto whitespace-pre-wrap font-mono text-xs text-silver">tools\audio-info.exe</pre>
              </template>
              <template #linux>
                <pre class="mt-2 overflow-x-auto whitespace-pre-wrap font-mono text-xs text-silver">pacmd list-sinks | grep "name:"
pactl info | grep Source</pre>
              </template>
              <template #macos>
                <div class="mt-2 space-y-1">
                  <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
                  <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>
                </div>
              </template>
            </PlatformLayout>
          </div>
        </div>

        <PlatformLayout :platform="platform">
          <template #windows>
            <div class="settings-subtle-surface space-y-3">
              <div class="section-kicker">Windows sink management</div>

              <div>
                <label for="virtual_sink" class="block text-sm font-medium text-storm mb-1">{{ $t('config.virtual_sink') }}</label>
                <input
                  id="virtual_sink"
                  v-model="config.virtual_sink"
                  type="text"
                  class="settings-input"
                  :placeholder="$t('config.virtual_sink_placeholder')"
                />
                <div class="text-sm text-storm mt-1 whitespace-pre-wrap">{{ $t('config.virtual_sink_desc') }}</div>
              </div>

              <div class="grid gap-3 xl:grid-cols-2">
                <Checkbox
                  id="install_steam_audio_drivers"
                  locale-prefix="config"
                  v-model="config.install_steam_audio_drivers"
                  default="true"
                ></Checkbox>

                <Checkbox
                  id="keep_sink_default"
                  locale-prefix="config"
                  v-model="config.keep_sink_default"
                  default="true"
                ></Checkbox>

                <Checkbox
                  id="auto_capture_sink"
                  locale-prefix="config"
                  v-model="config.auto_capture_sink"
                  default="true"
                ></Checkbox>
              </div>
            </div>
          </template>
        </PlatformLayout>
      </div>
    </details>

    <details class="settings-section settings-disclosure">
      <summary class="settings-disclosure-summary">
        <div>
          <div class="section-kicker">Display</div>
          <h3 class="settings-section-title mt-2">Outputs and fallback modes</h3>
          <div class="settings-summary-copy">Point Polaris at the right display and define how unsupported modes should fall back.</div>
        </div>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
      </summary>

      <div class="settings-disclosure-body settings-inline-stack">
        <div class="settings-subtle-surface space-y-4" data-display-resolution-planner>
          <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
            <div>
              <div class="section-kicker">Display Planner</div>
              <h4 class="mt-2 text-sm font-semibold text-silver">{{ $t('config.av_planner_title') }}</h4>
              <div class="mt-1 text-sm leading-relaxed text-storm">
                {{ $t('config.av_planner_copy', { aspect: displayPlanner.sourceAspectRatio }) }}
              </div>
            </div>
            <div class="rounded-2xl border border-success/25 bg-success/10 px-3 py-2 text-right text-sm text-success-bright">
              <div class="text-[10px] font-semibold uppercase tracking-eyebrow">{{ $t('config.av_planner_recommended_label', { title: displayPlanner.recommendedTitle }) }}</div>
              <div class="mt-1 font-medium">{{ displayPlanner.recommendedMode }}</div>
            </div>
          </div>

          <div class="grid gap-3 xl:grid-cols-3">
            <SelectableCard
              v-for="choice in displayPlanner.visibleChoices"
              :key="choice.id"
              :card-class="['min-h-[126px] rounded-lg border p-4 hover:border-storm/70', activeDisplayPlanId === choice.id
                ? 'border-ice bg-ice/12 shadow-[0_0_0_1px_rgba(224,230,237,0.18)]'
                : 'border-storm/30 bg-deep/40']"
              :selected="activeDisplayPlanId === choice.id"
              @click="applyDisplayPlan(choice)"
            >
              <div class="flex items-start justify-between gap-3">
                <div class="text-sm font-semibold text-silver">{{ choice.title }}</div>
                <span
                  class="shrink-0 rounded-full border px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow"
                  :class="choice.id === displayPlanner.recommendedId
                    ? 'border-success/30 bg-success/10 text-success'
                    : 'border-storm/30 bg-storm/10 text-storm'"
                >{{ choice.id === displayPlanner.recommendedId ? $t('config.av_planner_badge_recommended') : choice.badge }}</span>
              </div>
              <div class="mt-3 text-sm leading-relaxed text-storm">{{ choice.reason }}</div>
              <div class="mt-3">
                <div class="eyebrow-label">{{ $t('config.av_planner_target_mode') }}</div>
                <div class="mt-1 font-mono text-xs text-silver">{{ choice.targetMode }}</div>
              </div>
            </SelectableCard>
          </div>

          <div class="flex flex-col gap-3 rounded-lg border border-ice/15 bg-ice/5 p-4 text-sm text-storm lg:flex-row lg:items-center lg:justify-between">
            <div>
              <div class="font-medium text-silver">{{ $t('config.av_planner_moonlight_title') }}</div>
              <div class="mt-1">
                {{ $t('config.av_planner_moonlight_copy') }}
                <a href="https://papi-ux.com/docs/configuration/#common-options" target="_blank" class="focus-ring text-ice hover:underline">{{ $t('config.av_planner_moonlight_link') }}</a>
              </div>
            </div>
            <button type="button" class="focus-ring dashboard-action-button dashboard-action-button-secondary" @click="showDisplayPlannerAdvanced = !showDisplayPlannerAdvanced">
              {{ showDisplayPlannerAdvanced ? $t('config.av_planner_hide_advanced') : $t('config.av_planner_show_advanced') }}
            </button>
          </div>

          <div v-if="showDisplayPlannerAdvanced" class="settings-subtle-surface space-y-4" data-display-resolution-planner-advanced>
            <div>
              <div class="section-kicker">Advanced Display Planner</div>
              <div class="mt-2 text-sm leading-relaxed text-storm">
                {{ $t('config.av_planner_advanced_copy') }}
              </div>
            </div>
            <div class="grid gap-2 sm:grid-cols-2 xl:grid-cols-6">
              <SelectableCard
                v-for="factor in displayPlanner.advancedScaleFactors"
                :key="factor.label"
                :card-class="['rounded-lg border px-3 py-3', factor.safe ? 'border-storm/30 bg-deep/40 hover:border-storm/70' : 'border-warning/25 bg-warning/10']"
                :disabled="!factor.safe"
                :selected="factor.safe && customDisplayScale === factor.scaleFactor"
                @click="customDisplayScale = factor.scaleFactor"
              >
                <div class="text-sm font-semibold text-silver">{{ factor.label }}</div>
                <div class="mt-1 font-mono text-xs text-storm">{{ factor.targetMode }}</div>
                <div class="mt-1 text-[11px]" :class="factor.safe ? 'text-storm' : 'text-warning-bright'">{{ factor.safe ? $t('config.av_planner_factor_available') : $t('config.av_planner_factor_hidden') }}</div>
              </SelectableCard>
            </div>
            <label class="block text-sm font-medium text-storm">
              {{ $t('config.av_planner_custom_scale') }}
              <input
                v-model.number="customDisplayScale"
                type="number"
                min="0.5"
                max="2"
                step="0.25"
                class="settings-input mt-2"
              />
            </label>
          </div>
        </div>

        <DisplayOutputSelector
          :platform="platform"
          :config="config"
        />

        <div class="mb-3">
          <label for="fallback_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.fallback_mode') }}</label>
          <input
            id="fallback_mode"
            v-model="config.fallback_mode"
            type="text"
            class="settings-input"
            placeholder="1920x1080x60"
            @input="updateDisplayPlannerSource"
          />
          <div class="text-sm text-storm mt-1">{{ $t('config.fallback_mode_desc') }}</div>
          <p v-if="provenanceFor('fallback_mode')" class="mt-1 text-[11px] text-storm" data-provenance="fallback_mode">
            {{ $t('config.av_provenance_set_by', { source: provenanceFor('fallback_mode') }) }}
          </p>
        </div>

        <DisplayDeviceOptions
          :platform="platform"
          :config="config"
        />

        <div
          v-if="isWindows"
          class="settings-subtle-surface"
          :class="props.vdisplay ? 'border-warning/25' : 'border-success/25'"
        >
          <div class="flex items-center gap-2 text-sm font-medium text-silver">
            <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
            SudoVDA Driver
          </div>
          <div class="mt-2 text-sm text-storm">Current status: <span class="text-silver">{{ currentDriverStatus }}</span></div>
          <div v-if="props.vdisplay" class="mt-2 text-sm text-warning-bright">Install or update the SudoVDA driver if Polaris should be able to create Windows virtual displays reliably.</div>
        </div>

        <VirtualDisplayStatus
          :platform="platform"
          :config="config"
        />
      </div>
    </details>

    <details class="settings-section settings-disclosure">
      <summary class="settings-disclosure-summary">
        <div>
          <div class="section-kicker">Advanced Tuning</div>
          <h3 class="settings-section-title mt-2">Manual bitrate and pacing</h3>
          <div class="settings-summary-copy">Direct controls for bitrate ceilings, pacing floors, and the measured adaptive-bitrate range.</div>
        </div>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
      </summary>

      <div class="settings-disclosure-body settings-inline-stack">
        <div class="settings-form-grid">
          <div>
            <label for="max_bitrate" class="block text-sm font-medium text-storm mb-1">{{ $t("config.max_bitrate") }}</label>
            <input id="max_bitrate" v-model="config.max_bitrate" type="number" placeholder="0" class="settings-input" />
            <div class="text-sm text-storm mt-1">{{ $t("config.max_bitrate_desc") }}</div>
            <p v-if="provenanceFor('max_bitrate')" class="mt-1 text-[11px] text-storm" data-provenance="max_bitrate">
              {{ $t('config.av_provenance_set_by', { source: provenanceFor('max_bitrate') }) }}
            </p>
          </div>

          <div>
            <label for="minimum_fps_target" class="block text-sm font-medium text-storm mb-1">{{ $t("config.minimum_fps_target") }}</label>
            <input id="minimum_fps_target" v-model="config.minimum_fps_target" type="number" min="0" max="1000" placeholder="0" class="settings-input" />
            <div class="text-sm text-storm mt-1">{{ $t("config.minimum_fps_target_desc") }}</div>
          </div>

          <div>
            <label for="disconnect_resume_timeout_seconds" class="block text-sm font-medium text-storm mb-1">{{ $t("config.disconnect_resume_timeout_seconds") }}</label>
            <input id="disconnect_resume_timeout_seconds" v-model.number="config.disconnect_resume_timeout_seconds" type="number" min="0" max="86400" step="30" placeholder="300" class="settings-input" />
            <div class="text-sm text-storm mt-1">{{ $t("config.disconnect_resume_timeout_seconds_desc") }}</div>
          </div>
        </div>

        <div class="settings-subtle-surface space-y-3">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0">
              <div class="text-sm font-medium text-silver">{{ $t('config.av_adaptive_range_title') }}</div>
              <div class="mt-1 text-sm text-storm">{{ $t('config.av_adaptive_range_copy') }}</div>
            </div>
            <div class="control-chip whitespace-nowrap" :class="autoQualityTone">{{ autoQualityBadge }}</div>
          </div>

          <div v-if="autoQualityEnabled" class="settings-form-grid">
            <div>
              <label class="block text-sm font-medium text-storm mb-1">{{ $t('config.av_adaptive_range_min_label') }}</label>
              <input
                v-model.number="config.adaptive_bitrate_min"
                type="number"
                min="500"
                max="100000"
                step="500"
                class="settings-input text-sm"
              />
            </div>
            <div>
              <label class="block text-sm font-medium text-storm mb-1">{{ $t('config.av_adaptive_range_max_label') }}</label>
              <input
                v-model.number="config.adaptive_bitrate_max"
                type="number"
                min="1000"
                max="300000"
                step="1000"
                class="settings-input text-sm"
              />
            </div>
          </div>

          <div v-if="autoQualityEnabled" class="text-sm text-storm">
            {{ $t('config.av_adaptive_range_bounds', { floor: config.adaptive_bitrate_min / 1000, ceiling: config.adaptive_bitrate_max / 1000 }) }}
          </div>
          <div v-else class="text-sm text-storm">
            {{ $t('config.av_adaptive_range_disabled_hint') }}
          </div>
        </div>
      </div>
    </details>
  </div>
</template>

<style scoped>
</style>
