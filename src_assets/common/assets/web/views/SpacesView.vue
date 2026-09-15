<template>
  <div class="page-shell pb-2">
    <section class="page-header">
      <div class="page-heading">
        <h1 class="page-title">Spaces</h1>
        <p class="page-subtitle">Separate game libraries and Steam sign-ins on one PC.</p>
      </div>
      <div class="page-meta">
        <a href="https://github.com/papi-ux/polaris/blob/master/docs/spaces.md" target="_blank" rel="noopener noreferrer"
           class="focus-ring rounded-lg border border-ice/30 px-3 py-2 text-sm text-ice hover:underline">Spaces Guide</a>
        <span class="meta-pill">Preview</span>
      </div>
    </section>

    <section v-if="!snapshot?.profiles.some(space => !space.archived)" class="section-card" aria-labelledby="spaces-intro-title">
      <h2 id="spaces-intro-title" class="section-title">Create Your First Space</h2>
      <p class="mt-2 text-sm text-storm">Start with Host Setup below. The Spaces Guide covers installation and your first game.</p>
    </section>

    <p v-if="clientLoading" class="text-sm text-storm" role="status">Loading paired devices…</p>
    <p v-if="clientError" class="text-sm text-warning-bright" role="alert">{{ clientError }}</p>
    <MultiseatAssignments :clients="clients" :clients-ready="!clientLoading && !clientError" @snapshot="snapshot = $event" />
    <SpacesSetup />
    <div class="flex flex-wrap items-center justify-between gap-3 text-sm text-storm">
      <span>Pair new devices and manage their permissions in Devices.</span>
      <router-link to="/pin" class="focus-ring rounded px-1 py-2 text-ice hover:underline">Open Devices</router-link>
    </div>
  </div>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import SpacesSetup from '../components/SpacesSetup.vue'
import MultiseatAssignments from '../components/MultiseatAssignments.vue'
const clients = ref([]), snapshot = ref(null), clientError = ref('')
const clientLoading = ref(true)
const request = new AbortController()
onUnmounted(() => request.abort())
onMounted(async () => {
  const timeout = setTimeout(() => request.abort(), 12000)
  try {
    const response = await fetch('./api/clients/list', { credentials: 'include', cache: 'no-store', signal: request.signal })
    const next = await response.json()
    if (!response.ok || next?.status !== true || !Array.isArray(next.named_certs) ||
        !next.named_certs.every(client => client && typeof client.uuid === 'string' && client.uuid)) throw new Error()
    clients.value = next.named_certs
  } catch {
    clientError.value = 'Could not load paired devices. Refresh this page to manage device access.'
  } finally { clearTimeout(timeout); clientLoading.value = false }
})
</script>
