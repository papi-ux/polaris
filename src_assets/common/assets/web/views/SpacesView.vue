<template>
  <div class="page-shell pb-2">
    <section class="page-header">
      <div class="page-heading">
        <h1 class="page-title">Spaces</h1>
        <p class="page-subtitle">Your own place to play, on a shared gaming PC or a headless server.</p>
      </div>
      <div class="page-meta"><span class="meta-pill">Preview</span></div>
    </section>

    <section v-if="!snapshot?.profiles.length" class="section-card" aria-labelledby="spaces-intro-title">
      <h2 id="spaces-intro-title" class="section-title">One PC. Room for more.</h2>
      <p class="mt-2 max-w-3xl text-sm text-storm">
        Each space has its own Steam sign-in, saves, and settings. Give each player a space to play at the same time,
        or keep one space for your handheld and TV. You can also use a space on a gaming server without a monitor.
      </p>
      <p class="mt-3 text-sm text-storm">
        Already happy streaming your usual desktop and games? Keep using Library in Nova. Spaces are optional.
      </p>
    </section>

    <section v-if="snapshot?.profiles.length" class="section-card" aria-labelledby="your-spaces-title">
      <h2 id="your-spaces-title" class="section-title">Your spaces</h2>
      <div class="mt-4 grid gap-3 md:grid-cols-2 xl:grid-cols-3">
        <article v-for="space in snapshot.profiles" :key="space.id" class="min-w-0 rounded-xl border border-storm/20 bg-deep/40 p-4">
          <h3 class="break-words font-semibold text-silver">{{ space.name }}</h3>
          <p class="mt-1 text-sm text-storm">{{ space.steam ? 'Steam Big Picture' : 'Gaming space' }}</p>
          <p class="mt-3 text-xs text-storm">
            {{ space.clients.length ? space.clients.length + (space.clients.length === 1 ? ' assigned device' : ' assigned devices') : 'Assign a device below to get started.' }}
          </p>
        </article>
      </div>
      <p class="mt-4 text-sm text-storm">Open an assigned space from the game library in Nova. A space can stream to one device at a time.</p>
    </section>

    <SpacesSetup />
    <p v-if="clientLoading" class="text-sm text-storm" role="status">Loading paired devices…</p>
    <p v-if="clientError" class="text-sm text-warning-bright" role="alert">{{ clientError }}</p>
    <MultiseatAssignments :clients="clients" :clients-ready="!clientLoading && !clientError" @snapshot="snapshot = $event" />
    <div class="flex flex-wrap items-center justify-between gap-3 text-sm text-storm">
      <span>Pair new devices and manage their permissions in Devices.</span>
      <router-link to="/pin" class="focus-ring rounded px-1 py-2 text-ice hover:underline">Open Devices</router-link>
    </div>
    <details class="section-card text-sm text-storm">
      <summary class="focus-ring cursor-pointer rounded font-semibold text-silver">Steam accounts, games, and everyday streaming</summary>
      <p class="mt-3">Use one space per player. Each player signs in through Steam Big Picture the first time.
        Steam keeps that sign-in in the space, along with installed games and saves.</p>
      <p class="mt-3">To play Steam games at the same time, use separate Steam accounts and make sure each player has access
        to the game. Steam's account and library-sharing rules still apply.</p>
      <p class="mt-3">Standard streaming continues to open the usual apps on this PC.
        Nova's Streaming presets change stream settings such as resolution and frame rate; they do not switch Steam accounts.</p>
    </details>
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
