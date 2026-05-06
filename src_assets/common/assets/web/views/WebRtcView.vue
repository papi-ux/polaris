<template>
  <div class="space-y-5">
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Browser Stream</div>
        <h2 class="settings-section-title">WebRTC</h2>
      </div>

      <div class="grid gap-3 md:grid-cols-3">
        <div class="rounded-lg border border-storm/20 bg-deep/40 p-4">
          <div class="text-xs font-semibold uppercase text-storm">Build</div>
          <div class="mt-2 text-lg font-semibold text-silver">{{ status.build_enabled ? 'Enabled' : 'Not built' }}</div>
        </div>
        <div class="rounded-lg border border-storm/20 bg-deep/40 p-4">
          <div class="text-xs font-semibold uppercase text-storm">Config</div>
          <div class="mt-2 text-lg font-semibold text-silver">{{ status.config_enabled ? 'Enabled' : 'Disabled' }}</div>
        </div>
        <div class="rounded-lg border border-storm/20 bg-deep/40 p-4">
          <div class="text-xs font-semibold uppercase text-storm">Runtime</div>
          <div class="mt-2 text-lg font-semibold text-silver">{{ status.available ? 'Ready' : 'Pending' }}</div>
        </div>
      </div>

      <div class="mt-4 rounded-lg border border-amber-300/30 bg-amber-300/10 p-4 text-sm text-silver">
        {{ status.message || 'WebRTC browser streaming status is loading.' }}
      </div>

      <div class="mt-4 flex flex-wrap gap-2">
        <button
          type="button"
          class="focus-ring rounded-lg border border-storm/25 bg-deep px-4 py-2 text-sm font-medium text-silver transition-colors hover:border-ice/40 hover:text-ice"
          @click="refresh"
        >
          Refresh
        </button>
        <router-link
          to="/config"
          class="focus-ring rounded-lg bg-ice px-4 py-2 text-sm font-semibold text-void transition-colors hover:bg-ice/90"
        >
          Open Settings
        </router-link>
      </div>
    </section>
  </div>
</template>

<script setup>
import { onMounted, ref } from 'vue'

const status = ref({
  build_enabled: false,
  config_enabled: false,
  available: false,
  message: '',
})

async function refresh() {
  try {
    const response = await fetch('./api/webrtc/status', { credentials: 'include' })
    if (!response.ok) {
      status.value = {
        build_enabled: false,
        config_enabled: false,
        available: false,
        message: 'Unable to load WebRTC status.',
      }
      return
    }
    status.value = await response.json()
  } catch {
    status.value = {
      build_enabled: false,
      config_enabled: false,
      available: false,
      message: 'Unable to load WebRTC status.',
    }
  }
}

onMounted(refresh)
</script>
