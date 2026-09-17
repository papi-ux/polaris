<script setup>
import { ref, computed, onMounted, watch } from 'vue'
import {
  kscreenConnectorOptions,
  presentKscreenConnector,
  presentVirtualDisplayStatus,
} from '../../../virtual-display-status.js'

const props = defineProps({
  platform: String,
  config: Object,
  hostGeneration: Number,
})

const loading = ref(true)
const error = ref(null)
const vdStatus = ref(null)
const backends = ref([])
const displayOutputs = ref(null)
let requestGeneration = 0
const presentation = computed(() => presentVirtualDisplayStatus(vdStatus.value || {}))
const isKscreenBackend = computed(() => (
  vdStatus.value?.backend_detected === true && vdStatus.value?.backend === 'kscreen-doctor'
))
const connectorOptions = computed(() => kscreenConnectorOptions(displayOutputs.value?.outputs))
const connector = computed(() => presentKscreenConnector({
  selected: props.config?.linux_streaming_output,
  // display-outputs reports the connector the running host loaded; null when
  // that answer is missing, so unsaved edits are never shown as in use.
  loaded: typeof displayOutputs.value?.streaming_output === 'string' ? displayOutputs.value.streaming_output : null,
  available: vdStatus.value?.available === true,
  primary: props.config?.linux_primary_output,
  outputs: displayOutputs.value?.outputs,
}))

async function fetchStatus() {
  try {
    const resp = await fetch('./api/vdisplay/status', { credentials: 'include', cache: 'no-store' })
    if (resp.ok) return { data: await resp.json() }
    return { error: 'Failed to fetch virtual display status' }
  } catch (e) {
    return { error: 'Virtual display API not available' }
  }
}

async function fetchBackends() {
  try {
    const resp = await fetch('./api/vdisplay/backends', { credentials: 'include', cache: 'no-store' })
    if (resp.ok) {
      const data = await resp.json()
      return data.backends || []
    }
  } catch (e) {
    // Non-critical: backends list is supplementary
  }
  return null
}

async function fetchDisplayOutputs() {
  if (props.platform !== 'linux') return null
  try {
    const resp = await fetch('./api/linux/display-outputs', { credentials: 'include', cache: 'no-store' })
    if (!resp.ok) return null
    const data = await resp.json()
    return data?.status === true && Array.isArray(data.outputs) ? data : null
  } catch (e) {
    // Non-critical: connector hints are supplementary
    return null
  }
}

async function refresh() {
  const generation = ++requestGeneration
  loading.value = vdStatus.value === null
  const [status, backendList, outputs] = await Promise.all([
    fetchStatus(),
    fetchBackends(),
    fetchDisplayOutputs(),
  ])
  if (generation !== requestGeneration) return
  if (status.data) {
    vdStatus.value = status.data
    error.value = null
  } else if (vdStatus.value === null) {
    error.value = status.error
  }
  // A failed background refresh (the host may still be coming back up) keeps
  // the last answer, so an open connector field is never torn down by it.
  if (backendList) backends.value = backendList
  // Status and connectors land in the same tick, so a restart never renders
  // the new status beside the old connector list.
  if (status.data) displayOutputs.value = outputs
  loading.value = false
}

onMounted(refresh)
watch(() => props.hostGeneration, refresh)
</script>

<template>
  <div v-if="platform === 'linux'" class="mb-4">
    <div class="settings-subtle-surface space-y-3">
      <div>
        <div class="section-kicker">Linux backend status</div>
        <h3 class="mt-2 text-sm font-medium text-silver">Virtual Display</h3>
        <div class="mt-1 text-sm text-storm">Review which backend Polaris detected for virtual display creation and whether the current host can satisfy headless or managed display workflows.</div>
      </div>

      <div v-if="loading" class="text-sm text-storm">
        Detecting backends...
      </div>

      <div v-else-if="error" class="text-sm text-danger">
        {{ error }}
      </div>

      <template v-else-if="vdStatus">
        <div class="flex items-center gap-2">
          <span
            class="w-2 h-2 rounded-full"
            :class="presentation.kind === 'available'
              ? 'bg-success'
              : presentation.kind === 'missing'
                ? 'bg-danger'
                : 'bg-warning'"
          ></span>
          <span class="text-sm text-storm">
            {{ presentation.label }}
          </span>
        </div>

        <div class="text-sm text-storm">
          {{ presentation.detail }}
        </div>

        <div v-if="vdStatus.backend_detected" class="text-sm text-storm">
          Detected backend: <span class="text-silver font-medium">{{ vdStatus.backend }}</span>
        </div>

        <div v-if="backends.length > 0" class="mt-2 space-y-1">
          <div class="text-xs font-medium text-storm uppercase tracking-wide">Detected backends</div>
          <div
            v-for="b in backends"
            :key="b.id"
            class="flex items-center gap-2 text-sm"
          >
            <span
              class="w-1.5 h-1.5 rounded-full"
              :class="b.detected ? 'bg-success' : 'bg-storm/70'"
            ></span>
            <span :class="b.detected ? 'text-silver' : 'text-storm/60'">{{ b.name }}</span>
            <span v-if="b.detected" class="text-xs text-ice">(detected)</span>
          </div>
        </div>

        <div
          v-if="isKscreenBackend"
          class="mt-3 rounded-xl border border-storm/20 bg-deep/40 p-3 text-sm text-storm space-y-2"
          data-kscreen-configuration
        >
          <div class="text-silver font-medium text-xs uppercase tracking-wide">
            kscreen-doctor Configuration<span v-if="presentation.kind === 'unused'" class="normal-case"> (optional)</span>
          </div>
          <p v-if="presentation.kind === 'unused'" data-kscreen-optional>
            Private Stream does not use this. Set it only if you want to switch to Host Virtual Display.
          </p>
          <p>
            kscreen-doctor cannot add a new display. During a stream, Polaris turns on the connector you choose, makes it the primary screen, switches it to the client's resolution when the connector offers that mode, and puts the layout back afterward. Pick a spare connector with a dummy plug, not a monitor you use. For a real extra display, load EVDI with initial_device_count=1 or use a Hyprland session.
          </p>
          <label class="block text-xs font-medium text-storm">
            Streaming connector
            <select
              v-if="connectorOptions.length > 0"
              v-model="config.linux_streaming_output"
              data-kscreen-connector-select
              class="mt-1 w-full rounded-lg border border-storm/40 bg-void/40 px-3 py-2 font-mono text-sm text-silver focus:border-ice focus:outline-none"
            >
              <option value="">Choose a connector</option>
              <option v-for="option in connectorOptions" :key="option.name" :value="option.name">
                {{ option.label }}
              </option>
            </select>
            <input
              v-model="config.linux_streaming_output"
              data-kscreen-streaming-output
              type="text"
              class="w-full rounded-lg border border-storm/40 bg-void/40 px-3 py-2 font-mono text-sm text-silver focus:border-ice focus:outline-none"
              :class="connectorOptions.length > 0 ? 'mt-2' : 'mt-1'"
              :placeholder="connectorOptions.length > 0 ? 'or type the name, e.g. HDMI-A-2' : 'e.g. HDMI-A-2'"
            />
          </label>
          <p
            data-kscreen-connector-state
            :class="connector.kind === 'ready' ? 'text-success' : connector.kind === 'unset' ? 'text-storm' : 'text-ice'"
          >
            {{ connector.message }}
          </p>
          <p
            v-for="warning in connector.warnings"
            :key="warning"
            data-kscreen-connector-warning
            class="text-warning-bright"
          >
            {{ warning }}
          </p>
          <p class="text-xs">
            This saves as <code class="text-ice bg-void/50 px-1 rounded">linux_streaming_output</code>. It is separate from the general capture Output Name field.
          </p>
        </div>

        <div
          v-if="presentation.kind === 'missing'"
          class="mt-2 rounded-xl border border-storm/20 bg-deep/40 p-3 text-sm text-storm"
        >
          No virtual display backend was detected. Install one of the following only if you want to use Host Virtual Display:
          <ul class="list-disc list-inside mt-1 space-y-0.5">
            <li><span class="text-silver">EVDI</span> - kernel module + libevdi for true virtual connectors</li>
            <li><span class="text-silver">Wayland compositor</span> - Hyprland, Sway, or wlroots-based with headless output support</li>
            <li><span class="text-silver">kscreen-doctor</span> - KDE Plasma display management (fallback)</li>
          </ul>
        </div>
      </template>
    </div>
  </div>
</template>

<style scoped>
</style>
