<script setup>
import { ref, computed, onMounted } from 'vue'
import { presentVirtualDisplayStatus } from '../../../virtual-display-status.js'

defineProps({
  platform: String,
  config: Object,
})

const loading = ref(true)
const error = ref(null)
const vdStatus = ref(null)
const backends = ref([])
const presentation = computed(() => presentVirtualDisplayStatus(vdStatus.value || {}))

async function fetchStatus() {
  try {
    const resp = await fetch('./api/vdisplay/status', { credentials: 'include' })
    if (resp.ok) {
      vdStatus.value = await resp.json()
    } else {
      error.value = 'Failed to fetch virtual display status'
    }
  } catch (e) {
    error.value = 'Virtual display API not available'
  }
}

async function fetchBackends() {
  try {
    const resp = await fetch('./api/vdisplay/backends', { credentials: 'include' })
    if (resp.ok) {
      const data = await resp.json()
      backends.value = data.backends || []
    }
  } catch (e) {
    // Non-critical: backends list is supplementary
  }
}

onMounted(async () => {
  await Promise.all([fetchStatus(), fetchBackends()])
  loading.value = false
})
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
          v-if="presentation.kind === 'unconfigured' && vdStatus.backend === 'kscreen-doctor'"
          class="mt-3 rounded-xl border border-storm/20 bg-deep/40 p-3 text-sm text-storm space-y-2"
        >
          <div class="text-silver font-medium text-xs uppercase tracking-wide">kscreen-doctor Configuration</div>
          <p>
            This backend manages an existing output instead of creating one. Set
            <code class="text-ice bg-void/50 px-1 rounded">linux_streaming_output</code>
            in the Audio/Video settings above (Output Name) or in the config file so Polaris knows which output it may reconfigure.
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
