<script setup>
import { ref, computed, inject, watch, onMounted } from 'vue'
import { $tp } from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import VirtualDisplayStatus from "./audiovideo/VirtualDisplayStatus.vue";
import Checkbox from "../../Checkbox.vue";
import {
  applyStreamDisplayModeToConfig,
  resolveClientSettingsSync,
  resolveStreamDisplayMode,
  resolveStreamDisplayModeAvailability,
  resolveStreamDisplayRuntimeNotice,
} from '../../client-settings-sync'
import { buildResolutionPlanner } from '../../display-resolution-planner'

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
const streamDisplayModeDefinitions = [
  {
    id: 'headless_stream',
    title: 'Private Stream',
    badge: 'Recommended',
    group: 'private',
    copy: 'Recommended. Runs the game on a private display without touching the host monitors.',
    impact: 'Best for handheld play and most gaming PCs.',
    technical: 'Runtime: labwc · Capture: wlroots · Host display layout unchanged',
  },
  {
    id: 'windowed_stream',
    title: 'Private Stream (GPU-native)',
    badge: 'GPU-native',
    group: 'private',
    copy: 'The same private session, keeping frames on the GPU when the host supports it.',
    impact: 'Best for supported NVIDIA hosts; it may appear as a window on the host.',
    technical: 'Runtime: labwc · Capture: wlroots · Prefers DMA-BUF GPU frames',
  },
  {
    id: 'gamescope_stream',
    title: 'Gamescope Stream',
    badge: 'Deck-style',
    group: 'private',
    copy: 'Runs one game in a Steam Deck-style session that Polaris owns.',
    impact: 'Best for Steam-first hosts. Gamescope must be installed.',
    technical: 'Runtime: Gamescope · Capture: portal/PipeWire · Uses an idle or Polaris-owned compositor',
  },
  {
    id: 'host_virtual_display',
    title: 'Host Virtual Display',
    badge: 'Adds a display',
    group: 'host',
    copy: 'Adds a display to the host desktop and sizes it for the streaming client.',
    impact: 'The physical desktop stays usable, but windows and icons may rearrange.',
    technical: 'Runtime: host desktop · Backend: EVDI, wlroots, or KScreen · Adds and removes an output',
  },
  {
    id: 'headless_dongle',
    title: 'Headless Dongle',
    badge: 'Host default',
    group: 'host',
    copy: 'Moves the desktop onto a physical dummy plug. Privacy mode blanks the real panel after the one-time portal approval is saved; the panel stays on during that approval.',
    impact: 'Configure it once as the host default; Off mode keeps the real panel active, and clients cannot enable it for one game.',
    technical: 'Runtime: host desktop · Capture: portal by default, optional KMS · Requires streaming and primary outputs plus automatic display management',
  },
  {
    id: 'desktop_display',
    title: 'Mirror Desktop',
    badge: 'Visible on host',
    group: 'host',
    copy: 'Streams everything visible on the host desktop, including notifications.',
    impact: 'Best for remote-desktop use and quick checks; it provides no privacy isolation.',
    technical: 'Runtime: host desktop · Capture: portal/PipeWire · Uses the existing physical display',
  },
]

const streamDisplayModes = computed(() => streamDisplayModeDefinitions.map((mode) => ({
  ...mode,
  ...resolveStreamDisplayModeAvailability(mode.id, config.value.stream_display_mode_options),
})))

const plannedStreamDisplayModes = [
  {
    title: 'Family Mode (isolated)',
    copy: 'Separate per-person game sessions. Planned community work; not selectable yet.',
  },
  {
    title: 'Headless EVDI',
    copy: 'A dedicated headless EVDI desktop path. Planned community work; not selectable yet.',
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
  if (!clientSettingsSync.value.available) return 'Unavailable'
  if (clientSettingsSync.value.relaunchRequired) return 'Pending relaunch'
  return 'Bidirectional'
})
const clientSettingsSyncTone = computed(() => {
  if (!clientSettingsSync.value.available || clientSettingsSync.value.relaunchRequired) {
    return 'border-warning/30 bg-warning/10 text-warning-bright'
  }
  return 'border-success/30 bg-success/10 text-success'
})
const clientSettingsSyncCopy = computed(() => {
  if (!clientSettingsSync.value.available) {
    return 'Nova cannot see the Polaris client-settings endpoint on this host yet.'
  }
  if (clientSettingsSync.value.relaunchRequired) {
    return 'Nova and Polaris agree on the saved display choice, but the active stream is still using the previous runtime path.'
  }
  return 'Nova can push desired settings and Polaris reports the effective runtime state back to the client.'
})
const clientSettingsRows = computed(() => [
  { label: 'Display mode', value: clientSettingsSync.value.desiredModeLabel, note: 'Next stream' },
  { label: 'Effective mode', value: clientSettingsSync.value.effectiveModeLabel, note: clientSettingsSync.value.relaunchRequired ? 'Pending' : 'Synced' },
])

const autoQualityEnabled = computed(() => (
  config.value.ai_enabled === 'enabled' && config.value.adaptive_bitrate_enabled === 'enabled'
))
// The split state is reachable when the config file was edited by hand or an
// older host upgraded: exactly one of the pair is on.
const autoQualityPartial = computed(() => (
  (config.value.ai_enabled === 'enabled') !== (config.value.adaptive_bitrate_enabled === 'enabled')
))
const autoQualityBadge = computed(() => {
  if (autoQualityEnabled.value) return 'Auto Quality: On'
  if (autoQualityPartial.value) return 'Auto Quality: Partial'
  return 'Auto Quality: Manual'
})
const autoQualityTone = computed(() => {
  if (autoQualityEnabled.value) return 'border-success/30 bg-success/10 text-success'
  if (autoQualityPartial.value) return 'border-warning/30 bg-warning/10 text-warning-bright'
  return 'border-storm/40 bg-storm/10 text-storm'
})
const autoQualityCopy = computed(() => {
  if (autoQualityEnabled.value) {
    return 'Polaris will balance bitrate, per-game profile choice, and safer recovery targets without making the user choose a tuning layer.'
  }
  if (autoQualityPartial.value) {
    return 'This host has an older split Auto Quality state. Turn it on here to keep profile selection and live bitrate recovery together.'
  }
  return 'Manual tuning is active. Polaris will keep the selected bitrate and profile controls under Advanced Tuning.'
})
const autoQualityRows = computed(() => [
  {
    label: 'Profile',
    value: config.value.ai_enabled === 'enabled' ? 'Auto' : 'Manual',
    note: config.value.ai_enabled === 'enabled' ? 'Per game and device' : 'No launch tuning',
  },
  {
    label: 'Bitrate',
    value: config.value.adaptive_bitrate_enabled === 'enabled' ? 'Adaptive' : 'Fixed',
    note: config.value.adaptive_bitrate_enabled === 'enabled'
      ? `${Number(config.value.adaptive_bitrate_min || 0) / 1000}-${Number(config.value.adaptive_bitrate_max || 0) / 1000} Mbps`
      : `${Number(config.value.max_bitrate || 0) / 1000} Mbps cap`,
  },
  {
    label: 'Runtime',
    value: selectedStreamDisplayMode.value.title,
    note: selectedStreamDisplayMode.value.badge,
  },
  {
    label: 'Nova',
    value: clientSettingsSyncBadge.value,
    note: clientSettingsSync.value.relaunchRequired ? 'Relaunch to sync' : 'Push/pull ready',
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
      title: 'Pick a stream path',
      status: selectedStreamDisplayMode.value.title,
      copy: isGamescopePath.value
        ? 'Gamescope Stream: attach idle gamescope-0 or spawn owned headless; portal captures it. Encoder/bitrate/HDR below still apply.'
        : isDonglePath.value
          ? 'Dongle: set streaming + primary outputs, privacy swap, portal capture after topology prepare.'
          : isLabwcPath.value
            ? 'Private Stream (labwc) is the solid default — apps stay off the desk, wlroots capture.'
            : 'Mirror Desktop captures the host session via portal. Prefer Private Stream or Gamescope for isolated apps.',
    },
    {
      id: 'encoder',
      title: 'Encoder and quality',
      status: autoQualityBadge.value,
      copy: autoQualityEnabled.value
        ? 'Auto Quality balances bitrate and profile recovery for this path.'
        : 'Set encoder (NVENC/VAAPI), bitrate, and optional Auto Quality — these apply to labwc and gamescope.',
    },
  ]
  if (isLabwcPath.value) {
    items.push({
      id: 'wayland-vaapi',
      title: 'labwc GPU-native capture',
      status: config.value.linux_prefer_gpu_native_capture === 'enabled' ? 'GPU-native requested' : 'Safe default',
      copy: config.value.linux_prefer_gpu_native_capture === 'enabled'
        ? 'Windowed labwc may be used to keep DMA-BUF capture GPU-resident when proven.'
        : 'Leave GPU-native off unless session health shows SHM/system-memory fallback. This flag does not apply to Gamescope Stream.',
    })
  }
  if (isGamescopePath.value) {
    items.push({
      id: 'gamescope-host',
      title: 'Host gamescope stack',
      status: 'Portal + gamescope-0',
      copy: 'Needs gamescope on PATH and (on lea) private portal units. WebUI labwc flags (cage, GPU-native preference) are ignored for this path.',
    })
  }
  if (nvidiaTrueHeadlessGpuNativeGuard.value) {
    items.push({
      id: 'nvidia-headless-gpu-native-guard',
      title: 'NVIDIA true-headless guard',
      status: 'Needs GPU-native preference',
      copy: 'NVENC true-headless labwc hosts can hit cold-cache 503 when GPU-native capture is disabled. Switch to Private Stream (GPU-native) or enable the preference, restart, retry.',
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
              <button
                type="button"
                class="selectable-card focus-ring min-h-[132px] w-full p-4"
                :disabled="mode.available === false"
                :aria-pressed="streamDisplayMode === mode.id"
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
                  Unavailable: {{ mode.unavailableReason }}
                </span>
              </button>
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
              <div class="stat-tile-compact py-2.5">
                <div class="stat-kicker">Player impact</div>
                <div class="mt-1 text-xs leading-relaxed text-storm">{{ selectedStreamDisplayMode.impact }}</div>
              </div>
              <div class="stat-tile-compact py-2.5">
                <div class="stat-kicker">Capture path</div>
                <div class="mt-1 text-xs leading-relaxed text-storm">{{ selectedStreamDisplayMode.technical }}</div>
              </div>
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
              <div class="stat-tile-compact p-3" data-capture-path-explainer>
                <div class="text-sm font-semibold text-silver">How capture works</div>
                <p class="mt-1 text-xs leading-relaxed text-storm">
                  Polaris chooses capture after the launch mode is set. GPU-native keeps frames on the GPU; System-memory capture copies through RAM and can be the intended safe path on AMD and Intel. Mission Control reports the path actually used.
                </p>
              </div>

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
                  <div v-for="item in linuxStreamingSetupChecklist" :key="item.id" class="stat-tile-compact py-3">
                    <div class="flex items-start justify-between gap-3">
                      <div class="text-sm font-semibold text-silver">{{ item.title }}</div>
                      <span class="rounded-full border border-storm/30 bg-storm/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-storm">{{ item.status }}</span>
                    </div>
                    <div class="mt-2 text-sm leading-relaxed text-storm">{{ item.copy }}</div>
                  </div>
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
                  <div v-for="row in clientSettingsRows" :key="row.label" class="stat-tile-compact">
                    <div class="stat-kicker">{{ row.label }}</div>
                    <div class="mt-1 text-sm font-medium text-silver">{{ row.value }}</div>
                    <div class="mt-1 text-[11px] text-storm">{{ row.note }}</div>
                  </div>
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
                {{ autoQualityEnabled ? 'Auto Quality is balancing this host' : autoQualityPartial ? 'Auto Quality is partially enabled' : 'Manual stream tuning' }}
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
            {{ autoQualityEnabled ? 'Use Manual Tuning' : 'Enable Auto Quality' }}
          </button>
        </div>

        <div class="grid gap-2 sm:grid-cols-2 xl:grid-cols-4">
          <div v-for="row in autoQualityRows" :key="row.label" class="stat-tile-compact">
            <div class="stat-kicker">{{ row.label }}</div>
            <div class="mt-1 text-sm font-medium text-silver">{{ row.value }}</div>
            <div class="mt-1 text-[11px] text-storm">{{ row.note }}</div>
          </div>
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
              <h4 class="mt-2 text-sm font-semibold text-silver">Preset targets</h4>
              <div class="mt-1 text-sm leading-relaxed text-storm">
                Presets scale the saved fallback mode while keeping its {{ displayPlanner.sourceAspectRatio }} aspect ratio, so you choose a plain-language target instead of typing raw WxHxFPS values.
              </div>
            </div>
            <div class="rounded-2xl border border-success/25 bg-success/10 px-3 py-2 text-right text-sm text-success-bright">
              <div class="text-[10px] font-semibold uppercase tracking-eyebrow">Recommended · {{ displayPlanner.recommendedTitle }}</div>
              <div class="mt-1 font-medium">{{ displayPlanner.recommendedMode }}</div>
            </div>
          </div>

          <div class="grid gap-3 xl:grid-cols-3">
            <button
              v-for="choice in displayPlanner.visibleChoices"
              :key="choice.id"
              type="button"
              class="selectable-card focus-ring min-h-[126px] rounded-lg border p-4 hover:border-storm/70"
              :class="activeDisplayPlanId === choice.id
                ? 'border-ice bg-ice/12 shadow-[0_0_0_1px_rgba(224,230,237,0.18)]'
                : 'border-storm/30 bg-deep/40'"
              :aria-pressed="activeDisplayPlanId === choice.id"
              @click="applyDisplayPlan(choice)"
            >
              <div class="flex items-start justify-between gap-3">
                <div class="text-sm font-semibold text-silver">{{ choice.title }}</div>
                <span
                  class="shrink-0 rounded-full border px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow"
                  :class="choice.id === displayPlanner.recommendedId
                    ? 'border-success/30 bg-success/10 text-success'
                    : 'border-storm/30 bg-storm/10 text-storm'"
                >{{ choice.id === displayPlanner.recommendedId ? 'Recommended' : choice.badge }}</span>
              </div>
              <div class="mt-3 text-sm leading-relaxed text-storm">{{ choice.reason }}</div>
              <div class="mt-3">
                <div class="eyebrow-label">Target mode</div>
                <div class="mt-1 font-mono text-xs text-silver">{{ choice.targetMode }}</div>
              </div>
            </button>
          </div>

          <div class="flex flex-col gap-3 rounded-lg border border-ice/15 bg-ice/5 p-4 text-sm text-storm lg:flex-row lg:items-center lg:justify-between">
            <div>
              <div class="font-medium text-silver">Moonlight compatibility stays standard</div>
              <div class="mt-1">Planner choices only write the existing fallback display mode format. Nova/per-game overrides can layer on top where client-settings support exists.</div>
            </div>
            <button type="button" class="focus-ring dashboard-action-button dashboard-action-button-secondary" @click="showDisplayPlannerAdvanced = !showDisplayPlannerAdvanced">
              {{ showDisplayPlannerAdvanced ? 'Hide Advanced' : 'Show Advanced' }}
            </button>
          </div>

          <div v-if="showDisplayPlannerAdvanced" class="settings-subtle-surface space-y-4" data-display-resolution-planner-advanced>
            <div>
              <div class="section-kicker">Advanced Display Planner</div>
              <div class="mt-2 text-sm leading-relaxed text-storm">
                Scale factors are capped to 0.5x–2x and excessive modes stay hidden. Use Custom only when a client/game needs a specific override and the normal recommendation is not right.
              </div>
            </div>
            <div class="grid gap-2 sm:grid-cols-2 xl:grid-cols-6">
              <button
                v-for="factor in displayPlanner.advancedScaleFactors"
                :key="factor.label"
                type="button"
                class="selectable-card focus-ring rounded-lg border px-3 py-3"
                :class="factor.safe ? 'border-storm/30 bg-deep/40 hover:border-storm/70' : 'border-warning/25 bg-warning/10'"
                :disabled="!factor.safe"
                :aria-pressed="factor.safe && customDisplayScale === factor.scaleFactor"
                @click="customDisplayScale = factor.scaleFactor"
              >
                <div class="text-sm font-semibold text-silver">{{ factor.label }}</div>
                <div class="mt-1 font-mono text-xs text-storm">{{ factor.targetMode }}</div>
                <div class="mt-1 text-[11px]" :class="factor.safe ? 'text-storm' : 'text-warning-bright'">{{ factor.safe ? 'Available' : 'Hidden as excessive' }}</div>
              </button>
            </div>
            <label class="block text-sm font-medium text-storm">
              Custom scale factor
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
              <div class="text-sm font-medium text-silver">Adaptive bitrate range</div>
              <div class="mt-1 text-sm text-storm">Used only for evidence-backed live bitrate changes and gradual recovery.</div>
            </div>
            <div class="control-chip whitespace-nowrap" :class="autoQualityTone">{{ autoQualityBadge }}</div>
          </div>

          <div v-if="autoQualityEnabled" class="settings-form-grid">
            <div>
              <label class="block text-sm font-medium text-storm mb-1">Min Bitrate (kbps)</label>
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
              <label class="block text-sm font-medium text-storm mb-1">Max Bitrate (kbps)</label>
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
            Floor: {{ config.adaptive_bitrate_min / 1000 }} Mbps. Ceiling: {{ config.adaptive_bitrate_max / 1000 }} Mbps.
          </div>
          <div v-else class="text-sm text-storm">
            Enable Adaptive Bitrate above to use measured live bitrate recovery.
          </div>
        </div>
      </div>
    </details>
  </div>
</template>

<style scoped>
</style>
