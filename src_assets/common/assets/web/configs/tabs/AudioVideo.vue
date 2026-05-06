<script setup>
import { ref, computed, inject } from 'vue'
import { $tp } from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import VirtualDisplayStatus from "./audiovideo/VirtualDisplayStatus.vue";
import Checkbox from "../../Checkbox.vue";

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

const streamDisplayModes = [
  {
    id: 'headless_stream',
    title: 'Headless Stream',
    badge: 'Recommended',
    copy: 'Recommended. Starts apps inside a private hidden compositor. It does not mirror your current desktop.',
    note: 'Best isolation path. On some wlroots headless outputs this can fall back to SHM/RAM capture instead of DMA-BUF.',
    restartCopy: 'Requires restart. Polaris will stream an isolated hidden runtime, not the existing KDE or GNOME session.',
  },
  {
    id: 'host_virtual_display',
    title: 'Host Virtual Display',
    badge: 'Compatibility',
    copy: 'Compatibility mode. Creates a display the operating system can see. Your desktop may rearrange or remember it.',
    note: 'Use this when the hidden compositor path is not available or the desktop needs to own the virtual output.',
    restartCopy: 'Requires restart. KDE, GNOME, or display tools may see this as a monitor.',
  },
  {
    id: 'desktop_display',
    title: 'Desktop Display',
    badge: 'Visible desktop',
    copy: 'Streams from your current desktop session when you want the visible KDE, GNOME, or wlroots desktop.',
    note: 'Useful for quick validation, but it is not isolated from the host desktop.',
    restartCopy: 'Requires restart. Polaris will stream from the current desktop session.',
  },
  {
    id: 'windowed_stream',
    title: 'GPU-Native Test',
    badge: 'Experimental',
    copy: 'Requests a private stream runtime and allows Polaris to run it windowed when that is needed to preserve DMA-BUF/CUDA capture.',
    note: 'This is the fast-path validation mode. It can look less "headless" on purpose so capture stays GPU-resident.',
    restartCopy: 'Requires restart. GPU-Native Test may override hidden headless into a windowed labwc runtime to avoid the SHM/RAM copy path.',
  },
]

const streamDisplayMode = computed(() => {
  const headless = config.value.headless_mode === 'enabled'
  const cage = config.value.linux_use_cage_compositor === 'enabled'
  const gpuNative = config.value.linux_prefer_gpu_native_capture === 'enabled'

  if (headless && cage && gpuNative) return 'windowed_stream'
  if (headless && cage) return 'headless_stream'
  if (headless) return 'host_virtual_display'
  return 'desktop_display'
})

const selectedStreamDisplayMode = computed(() => (
  streamDisplayModes.find((mode) => mode.id === streamDisplayMode.value) || streamDisplayModes[0]
))

function setEnabledConfig(key, enabled) {
  config.value[key] = enabled ? 'enabled' : 'disabled'
}

function setStreamDisplayMode(mode) {
  switch (mode) {
    case 'headless_stream':
      setEnabledConfig('headless_mode', true)
      setEnabledConfig('linux_use_cage_compositor', true)
      setEnabledConfig('linux_prefer_gpu_native_capture', false)
      break
    case 'host_virtual_display':
      setEnabledConfig('headless_mode', true)
      setEnabledConfig('linux_use_cage_compositor', false)
      setEnabledConfig('linux_prefer_gpu_native_capture', false)
      break
    case 'desktop_display':
      setEnabledConfig('headless_mode', false)
      setEnabledConfig('linux_use_cage_compositor', false)
      setEnabledConfig('linux_prefer_gpu_native_capture', false)
      break
    case 'windowed_stream':
      setEnabledConfig('headless_mode', true)
      setEnabledConfig('linux_use_cage_compositor', true)
      setEnabledConfig('linux_prefer_gpu_native_capture', true)
      break
  }
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
</script>

<template>
  <div id="audio-video" class="config-page">
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Capture</div>
        <h3 class="settings-section-title">Launch mode and capture path</h3>
      </div>

      <div class="settings-inline-stack">
        <div v-if="isLinux" class="settings-subtle-surface space-y-4">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0">
              <div class="section-kicker">Stream Display Mode</div>
              <div class="mt-2 text-sm text-storm">Choose where Polaris starts and captures Linux sessions.</div>
            </div>
            <span class="rounded-full border border-ice/20 bg-ice/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.16em] text-ice">
              {{ selectedStreamDisplayMode.title }}
            </span>
          </div>

          <div class="grid gap-3 xl:grid-cols-2">
            <button
              v-for="mode in streamDisplayModes"
              :key="mode.id"
              type="button"
              class="focus-ring min-h-[112px] rounded-lg border p-4 text-left transition"
              :class="streamDisplayMode === mode.id ? 'border-ice/60 bg-ice/10' : 'border-storm/40 bg-deep/40 hover:border-storm/70'"
              @click="setStreamDisplayMode(mode.id)"
            >
              <div class="flex items-start justify-between gap-3">
                <div class="text-sm font-semibold text-silver">{{ mode.title }}</div>
                <span
                  class="shrink-0 rounded-full border px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.16em]"
                  :class="mode.id === 'headless_stream'
                    ? 'border-green-400/30 bg-green-400/10 text-green-300'
                    : mode.id === 'windowed_stream'
                      ? 'border-amber-300/40 bg-amber-300/10 text-amber-200'
                      : 'border-storm/40 bg-storm/10 text-storm'"
                >
                  {{ mode.badge }}
                </span>
              </div>
              <div class="mt-3 text-sm leading-relaxed text-storm">{{ mode.copy }}</div>
              <div class="mt-3 rounded-md border border-storm/20 bg-void/25 px-2.5 py-2 text-xs leading-relaxed text-storm">
                {{ mode.note }}
              </div>
            </button>
          </div>

          <div class="settings-warning-surface">
            {{ selectedStreamDisplayMode.restartCopy }}
          </div>

          <div class="grid gap-3 xl:grid-cols-3">
            <div class="surface-muted p-3">
              <div class="text-xs font-semibold uppercase tracking-[0.16em] text-accent">Isolation</div>
              <div class="mt-2 text-sm leading-relaxed text-storm">Headless Stream keeps apps off your real desktop and is the default stability target.</div>
            </div>
            <div class="surface-muted p-3">
              <div class="text-xs font-semibold uppercase tracking-[0.16em] text-green-300">GPU path</div>
              <div class="mt-2 text-sm leading-relaxed text-storm">GPU-Native Test favors DMA-BUF and CUDA/NVENC residency, even if that means a visible labwc window.</div>
            </div>
            <div class="surface-muted p-3">
              <div class="text-xs font-semibold uppercase tracking-[0.16em] text-amber-200">FPS target</div>
              <div class="mt-2 text-sm leading-relaxed text-storm">A 120 FPS client target still needs the game/output to render above 60 FPS; Polaris will show the live gap on the dashboard.</div>
            </div>
          </div>

          <details class="settings-disclosure rounded-lg border border-storm/30 bg-deep/30">
            <summary class="settings-disclosure-summary p-4">
              <div>
                <div class="section-kicker">Advanced</div>
                <h4 class="mt-2 text-sm font-semibold text-silver">Advanced Linux runtime flags</h4>
                <div class="mt-1 text-sm text-storm">Direct access to the existing config keys used by the mode selector.</div>
              </div>
              <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
            </summary>

            <div class="grid gap-4 p-4 pt-0 xl:grid-cols-2">
              <div class="surface-muted p-4">
                <div class="text-sm font-medium text-silver">Hidden-output request</div>
                <div class="mt-1 text-sm text-storm">Existing headless_mode config key. Enabled by Headless Stream and Host Virtual Display.</div>
                <div class="mt-3 rounded bg-deep/60 px-2 py-1 font-mono text-xs text-storm">headless_mode</div>
                <label class="mt-4 flex items-center justify-between gap-4">
                  <span class="text-xs uppercase tracking-[0.18em] text-storm">Requested</span>
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
                <div class="mt-1 text-sm text-storm">Existing linux_use_cage_compositor config key. Enabled by Headless Stream and Windowed Stream.</div>
                <div class="mt-3 rounded bg-deep/60 px-2 py-1 font-mono text-xs text-storm">linux_use_cage_compositor</div>
                <label class="mt-4 flex items-center justify-between gap-4">
                  <span class="text-xs uppercase tracking-[0.18em] text-storm">Use labwc</span>
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
                <div class="mt-1 text-sm text-storm">Existing linux_prefer_gpu_native_capture config key. Enabled by GPU-Native Test. When active, Polaris may run labwc windowed instead of truly hidden so capture can stay on the GPU.</div>
                <div class="mt-3 rounded bg-deep/60 px-2 py-1 font-mono text-xs text-storm">linux_prefer_gpu_native_capture</div>
                <label class="mt-4 flex items-center justify-between gap-4">
                  <span class="text-xs uppercase tracking-[0.18em] text-storm">Experimental</span>
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
                  <span class="text-xs uppercase tracking-[0.18em] text-storm">Diagnostics</span>
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
            class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
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
                  class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
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
            class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
            placeholder="1920x1080x60"
            @input="validateFallbackMode"
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
          :class="props.vdisplay ? 'border-amber-300/25' : 'border-green-400/25'"
        >
          <div class="flex items-center gap-2 text-sm font-medium text-silver">
            <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
            SudoVDA Driver
          </div>
          <div class="mt-2 text-sm text-storm">Current status: <span class="text-silver">{{ currentDriverStatus }}</span></div>
          <div v-if="props.vdisplay" class="mt-2 text-sm text-amber-100">Install or update the SudoVDA driver if Polaris should be able to create Windows virtual displays reliably.</div>
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
          <div class="section-kicker">Delivery</div>
          <h3 class="settings-section-title mt-2">Bitrate and pacing</h3>
          <div class="settings-summary-copy">Set the bitrate ceiling, pacing floor, and how the host reacts to network changes.</div>
        </div>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
      </summary>

      <div class="settings-disclosure-body settings-inline-stack">
        <div class="settings-form-grid">
          <div>
            <label for="max_bitrate" class="block text-sm font-medium text-storm mb-1">{{ $t("config.max_bitrate") }}</label>
            <input id="max_bitrate" v-model="config.max_bitrate" type="number" placeholder="0" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" />
            <div class="text-sm text-storm mt-1">{{ $t("config.max_bitrate_desc") }}</div>
          </div>

          <div>
            <label for="minimum_fps_target" class="block text-sm font-medium text-storm mb-1">{{ $t("config.minimum_fps_target") }}</label>
            <input id="minimum_fps_target" v-model="config.minimum_fps_target" type="number" min="0" max="1000" placeholder="0" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" />
            <div class="text-sm text-storm mt-1">{{ $t("config.minimum_fps_target_desc") }}</div>
          </div>
        </div>

        <div class="settings-subtle-surface space-y-3">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0">
              <div class="text-sm font-medium text-silver">Adaptive Bitrate</div>
              <div class="mt-1 text-sm text-storm">Reduce bitrate on loss or RTT spikes, then recover gradually.</div>
            </div>
            <label class="relative inline-flex shrink-0 items-center cursor-pointer">
              <input
                type="checkbox"
                class="sr-only peer"
                :checked="config.adaptive_bitrate_enabled === 'enabled'"
                @change="config.adaptive_bitrate_enabled = $event.target.checked ? 'enabled' : 'disabled'"
              />
              <div class="h-5 w-9 rounded-full bg-storm/40 transition-colors peer peer-checked:bg-accent after:absolute after:left-[2px] after:top-[2px] after:h-4 after:w-4 after:rounded-full after:bg-white after:transition-all after:content-[''] peer-checked:after:translate-x-full"></div>
            </label>
          </div>

          <div v-if="config.adaptive_bitrate_enabled === 'enabled'" class="settings-form-grid">
            <div>
              <label class="block text-sm font-medium text-storm mb-1">Min Bitrate (kbps)</label>
              <input
                v-model.number="config.adaptive_bitrate_min"
                type="number"
                min="500"
                max="100000"
                step="500"
                class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver text-sm focus:border-ice focus:outline-none"
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
                class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver text-sm focus:border-ice focus:outline-none"
              />
            </div>
          </div>

          <div v-if="config.adaptive_bitrate_enabled === 'enabled'" class="text-sm text-storm">
            Floor: {{ config.adaptive_bitrate_min / 1000 }} Mbps. Ceiling: {{ config.adaptive_bitrate_max / 1000 }} Mbps.
          </div>
        </div>
      </div>
    </details>
  </div>
</template>

<style scoped>
</style>
