<script setup>
import { ref, onMounted } from 'vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const loading = ref(true)
const error = ref(null)
const vdStatus = ref(null)
const backends = ref([])

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
    <!-- Virtual Display Status Card -->
    <div class="bg-twilight rounded-xl border border-storm p-4 space-y-3">
      <h3 class="text-lg font-semibold text-silver">Virtual Display</h3>

      <!-- Loading state -->
      <div v-if="loading" class="text-sm text-storm">
        Detecting backends...
      </div>

      <!-- Error state -->
      <div v-else-if="error" class="text-sm text-red-400">
        {{ error }}
      </div>

      <!-- Status display -->
      <template v-else-if="vdStatus">
        <!-- Availability indicator -->
        <div class="flex items-center gap-2">
          <span
            class="w-2 h-2 rounded-full"
            :class="vdStatus.available ? 'bg-green-400' : 'bg-red-400'"
          ></span>
          <span class="text-sm text-storm">
            {{ vdStatus.available ? 'Available' : 'Not available' }}
          </span>
        </div>

        <!-- Active backend -->
        <div v-if="vdStatus.available" class="text-sm text-storm">
          Active backend: <span class="text-silver font-medium">{{ vdStatus.backend }}</span>
        </div>

        <!-- Backends list -->
        <div v-if="backends.length > 0" class="mt-2 space-y-1">
          <div class="text-xs font-medium text-storm uppercase tracking-wide">Backends</div>
          <div
            v-for="b in backends"
            :key="b.id"
            class="flex items-center gap-2 text-sm"
          >
            <span
              class="w-1.5 h-1.5 rounded-full"
              :class="b.detected ? 'bg-green-400' : 'bg-gray-600'"
            ></span>
            <span :class="b.detected ? 'text-silver' : 'text-storm/60'">{{ b.name }}</span>
            <span v-if="b.detected" class="text-xs text-ice">(active)</span>
          </div>
        </div>

        <!-- kscreen-doctor config hint -->
        <div
          v-if="vdStatus.backend === 'kscreen-doctor'"
          class="mt-3 p-3 bg-deep rounded-lg border border-storm/50 text-sm text-storm space-y-2"
        >
          <div class="text-silver font-medium text-xs uppercase tracking-wide">kscreen-doctor Configuration</div>
          <p>
            This backend manages existing physical displays. Make sure
            <code class="text-ice bg-void/50 px-1 rounded">linux_streaming_output</code> and
            <code class="text-ice bg-void/50 px-1 rounded">linux_primary_output</code>
            are configured in the Audio/Video settings above (Output Name field) or in the config file.
          </p>
        </div>

        <!-- No backend hint -->
        <div
          v-if="!vdStatus.available"
          class="mt-2 p-3 bg-deep rounded-lg border border-storm/50 text-sm text-storm"
        >
          No virtual display backend was detected. Install one of the following:
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
