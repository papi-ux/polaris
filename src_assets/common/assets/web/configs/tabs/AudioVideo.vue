<script setup>
import {ref, computed, inject} from 'vue'
import {$tp} from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";
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

    <!-- Headless Mode -->
    <div class="border border-accent/30 rounded-lg p-4 mb-4 bg-accent/5">
      <div class="flex items-center justify-between">
        <div>
          <div class="text-sm font-medium text-silver flex items-center gap-2">
            Headless Mode
            <span class="px-1.5 py-0.5 text-[9px] font-bold rounded-full bg-accent/20 text-accent">NEW</span>
          </div>
          <div class="text-xs text-storm mt-1">Request invisible streaming by running games inside a headless labwc compositor instead of a visible desktop window.</div>
        </div>
        <label class="relative inline-flex items-center cursor-pointer">
          <input type="checkbox" class="sr-only peer"
                 :checked="config.headless_mode === 'enabled'"
                 @change="config.headless_mode = $event.target.checked ? 'enabled' : 'disabled'" />
          <div class="w-9 h-5 bg-storm/40 peer-focus:outline-none rounded-full peer peer-checked:bg-accent transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full"></div>
        </label>
      </div>
      <div v-if="config.headless_mode === 'enabled'" class="text-xs text-accent/80 mt-2">
        Requires restart. Eliminates: KWin window rules, edit mode watchdog, display preview, focus stealing.
      </div>

      <div v-if="platform === 'linux'" class="mt-4 pt-4 border-t border-accent/20">
        <div class="flex items-center justify-between gap-4">
          <div>
            <div class="text-sm font-medium text-silver">Prefer GPU-Native Capture</div>
            <div class="text-xs text-storm mt-1">
              Keep DMA-BUF / GPU-native capture paths as the priority. If headless mode would fall back to SHM,
              Polaris can start labwc windowed instead so NVENC or VAAPI stays on the fast path.
            </div>
          </div>
          <label class="relative inline-flex items-center cursor-pointer shrink-0">
            <input
              type="checkbox"
              class="sr-only peer"
              :checked="config.linux_prefer_gpu_native_capture === 'enabled'"
              @change="config.linux_prefer_gpu_native_capture = $event.target.checked ? 'enabled' : 'disabled'"
            />
            <div class="w-9 h-5 bg-storm/40 peer-focus:outline-none rounded-full peer peer-checked:bg-accent transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full"></div>
          </label>
        </div>
        <div class="text-xs text-storm mt-2">
          Enabled: performance wins, even if the compositor becomes visible. Disabled: invisibility wins, even if capture falls back to SHM.
        </div>

        <div class="flex items-center justify-between gap-4 mt-4 pt-4 border-t border-accent/20">
          <div>
            <div class="text-sm font-medium text-silver">Capture Telemetry Profiling</div>
            <div class="text-xs text-storm mt-1">
              Record Linux capture dispatch and ingest timings, then emit a transport-tagged p50/p99 summary every 600 frames.
              Useful for comparing SHM fallback against DMA-BUF paths without enabling per-frame spam.
            </div>
          </div>
          <label class="relative inline-flex items-center cursor-pointer shrink-0">
            <input
              type="checkbox"
              class="sr-only peer"
              :checked="config.linux_capture_profile === 'enabled'"
              @change="config.linux_capture_profile = $event.target.checked ? 'enabled' : 'disabled'"
            />
            <div class="w-9 h-5 bg-storm/40 peer-focus:outline-none rounded-full peer peer-checked:bg-accent transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full"></div>
          </label>
        </div>
        <div class="text-xs text-storm mt-2">
          Disabled by default. When enabled, summaries are emitted to the Polaris log under <code>capture_telemetry</code>.
        </div>
      </div>
    </div>

    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="block text-sm font-medium text-storm mb-1">{{ $t('config.audio_sink') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="audio_sink"
             :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
             v-model="config.audio_sink" />
      <div class="text-sm text-storm mt-1 whitespace-pre-wrap">
        {{ $tp('config.audio_sink_desc') }}<br>
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>

    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3">
          <label for="virtual_sink" class="block text-sm font-medium text-storm mb-1">{{ $t('config.virtual_sink') }}</label>
          <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="virtual_sink" :placeholder="$t('config.virtual_sink_placeholder')"
                 v-model="config.virtual_sink" />
          <div class="text-sm text-storm mt-1 whitespace-pre-wrap">{{ $t('config.virtual_sink_desc') }}</div>
        </div>
        <!-- Install Steam Audio Drivers -->
        <Checkbox class="mb-3"
                  id="install_steam_audio_drivers"
                  locale-prefix="config"
                  v-model="config.install_steam_audio_drivers"
                  default="true"
        ></Checkbox>

        <Checkbox class="mb-3"
                  id="keep_sink_default"
                  locale-prefix="config"
                  v-model="config.keep_sink_default"
                  default="true"
        ></Checkbox>

        <Checkbox class="mb-3"
                  id="auto_capture_sink"
                  locale-prefix="config"
                  v-model="config.auto_capture_sink"
                  default="true"
        ></Checkbox>
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <Checkbox class="mb-3"
              id="stream_audio"
              locale-prefix="config"
              v-model="config.stream_audio"
              default="true"
    ></Checkbox>

    <AdapterNameSelector
        :platform="platform"
        :config="config"
    />

    <DisplayOutputSelector
      :platform="platform"
      :config="config"
    />

    <DisplayDeviceOptions
      :platform="platform"
      :config="config"
    />

    <!-- Display Modes -->
    <DisplayModesSettings
        :platform="platform"
        :config="config"
    />

    <!-- Fallback Display Mode -->
    <div class="mb-3">
      <label for="fallback_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.fallback_mode') }}</label>
      <input
        type="text"
        class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
        id="fallback_mode"
        v-model="config.fallback_mode"
        placeholder="1920x1080x60"
        @input="validateFallbackMode"
      />
      <div class="text-sm text-storm mt-1">{{ $t('config.fallback_mode_desc') }}</div>
    </div>

    <!-- Headless Mode -->
    <Checkbox class="mb-3"
              id="headless_mode"
              locale-prefix="config"
              v-model="config.headless_mode"
              default="false"
              v-if="platform === 'windows'"
    ></Checkbox>

    <!-- Double Refreshrate -->
    <Checkbox class="mb-3"
              id="double_refreshrate"
              locale-prefix="config"
              v-model="config.double_refreshrate"
              default="false"
              v-if="platform === 'windows'"
    ></Checkbox>

    <!-- Isolated Virtual Display -->
    <Checkbox class="mb-3"
              id="isolated_virtual_display_option"
              locale-prefix="config"
              v-model="config.isolated_virtual_display_option"
              default="false"
              v-if="platform === 'windows'"
    ></Checkbox>

    <!-- SudoVDA Driver Status (Windows) -->
    <div class="border-l-4 p-3 rounded-lg" :class="[vdisplay ? 'bg-twilight/50 border-yellow-500 text-silver' : 'bg-twilight/50 border-green-500 text-silver']" v-if="platform === 'windows'">
      <svg class="w-5 h-5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg> SudoVDA Driver status: {{currentDriverStatus}}
    </div>
    <div class="text-sm text-storm mt-1" v-if="platform === 'windows' && vdisplay">Please ensure SudoVDA driver is installed to the latest version and enabled properly.</div>

    <!-- Adaptive Bitrate -->
    <div class="border border-storm/30 rounded-lg p-4 space-y-3 mt-4">
      <div class="flex items-center justify-between">
        <div>
          <div class="text-sm font-medium text-silver">Adaptive Bitrate</div>
          <div class="text-xs text-storm">Automatically adjust stream bitrate based on network conditions</div>
        </div>
        <label class="relative inline-flex items-center cursor-pointer">
          <input type="checkbox" class="sr-only peer"
                 :checked="config.adaptive_bitrate_enabled === 'enabled'"
                 @change="config.adaptive_bitrate_enabled = $event.target.checked ? 'enabled' : 'disabled'" />
          <div class="w-9 h-5 bg-storm/40 peer-focus:outline-none rounded-full peer peer-checked:bg-accent transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full"></div>
        </label>
      </div>

      <div v-if="config.adaptive_bitrate_enabled === 'enabled'" class="grid grid-cols-2 gap-3">
        <div>
          <label class="block text-xs text-storm mb-1">Min Bitrate (kbps)</label>
          <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-1.5 text-silver text-sm focus:border-ice focus:outline-none"
                 v-model.number="config.adaptive_bitrate_min" min="500" max="100000" step="500" />
        </div>
        <div>
          <label class="block text-xs text-storm mb-1">Max Bitrate (kbps)</label>
          <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-1.5 text-silver text-sm focus:border-ice focus:outline-none"
                 v-model.number="config.adaptive_bitrate_max" min="1000" max="300000" step="1000" />
        </div>
      </div>

      <div v-if="config.adaptive_bitrate_enabled === 'enabled'" class="text-xs text-storm">
        Reduces bitrate on packet loss or RTT spikes, then gradually recovers. Floor: {{ config.adaptive_bitrate_min / 1000 }} Mbps, Ceiling: {{ config.adaptive_bitrate_max / 1000 }} Mbps.
      </div>
    </div>

    <!-- Virtual Display Status (Linux) -->
    <VirtualDisplayStatus
      :platform="platform"
      :config="config"
    />

  </div>
</template>

<style scoped>
</style>
