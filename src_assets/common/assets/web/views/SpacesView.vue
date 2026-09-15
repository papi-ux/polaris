<template>
  <div class="page-shell pb-2">
    <section class="page-header">
      <div class="page-heading">
        <h1 class="page-title">Spaces</h1>
        <p class="page-subtitle">Your own place to play, on a shared gaming PC or a headless server.</p>
      </div>
      <div class="page-meta"><span class="meta-pill">Preview</span></div>
    </section>

    <section v-if="!snapshot?.profiles.some(space => !space.archived)" class="section-card" aria-labelledby="spaces-intro-title">
      <h2 id="spaces-intro-title" class="section-title">One PC. Room for more.</h2>
      <p class="mt-2 max-w-3xl text-sm text-storm">
        Each space has its own Steam sign-in, saves, and settings. Give each player a space to play at the same time,
        or keep one space for your handheld and TV. You can also use a space on a gaming server without a monitor.
      </p>
      <p class="mt-3 text-sm text-storm">
        Already happy streaming your usual desktop and games? Keep using Library in Nova. Spaces are optional.
      </p>
    </section>

    <p v-if="clientLoading" class="text-sm text-storm" role="status">Loading paired devices…</p>
    <p v-if="clientError" class="text-sm text-warning-bright" role="alert">{{ clientError }}</p>
    <MultiseatAssignments :clients="clients" :clients-ready="!clientLoading && !clientError" @snapshot="snapshot = $event" />
    <SpacesSetup />
    <details class="section-card" :open="!snapshot?.profiles.some(space => !space.archived)">
      <summary class="focus-ring cursor-pointer rounded py-2 font-semibold text-silver">First Play Checklist</summary>
      <ol class="mt-4 grid gap-4 sm:grid-cols-2">
        <li><h2 class="font-semibold text-silver">1. Check Host</h2><p class="mt-1 text-sm text-storm">Open Host Setup. Complete the Docker, graphics, controls, and security checks on the PC running Polaris.</p></li>
        <li><h2 class="font-semibold text-silver">2. Prepare Space</h2><p class="mt-1 text-sm text-storm">Prepare the runtime and name your Space for a player or room. Allow your device under Device Access and choose its Default Space.</p></li>
        <li><h2 class="font-semibold text-silver">3. Sign In To Steam</h2><p class="mt-1 text-sm text-storm">In Nova, open this host’s Library. Use Change Space if needed, then open Steam Big Picture to sign in and install a game.</p></li>
        <li><h2 class="font-semibold text-silver">4. Test Controls And Sound</h2><p class="mt-1 text-sm text-storm">Start at 60 FPS. Check both sticks, buttons, picture, and sound in gameplay. Save before disconnecting; leaving ends the game session.</p></li>
      </ol>
      <p class="mt-4 text-xs text-storm">Host checks confirm setup prerequisites. The gameplay check is yours to confirm. Steam account selection happens inside Steam Big Picture.</p>
    </details>
    <div class="flex flex-wrap items-center justify-between gap-3 text-sm text-storm">
      <span>Pair new devices and manage their permissions in Devices.</span>
      <router-link to="/pin" class="focus-ring rounded px-1 py-2 text-ice hover:underline">Open Devices</router-link>
    </div>
    <details class="section-card text-sm text-storm">
      <summary class="focus-ring cursor-pointer rounded font-semibold text-silver">Steam Accounts, Games, And Everyday Streaming</summary>
      <p class="mt-3">Use one space per player. Each player signs in through Steam Big Picture the first time.
        Steam keeps that sign-in in the space, along with installed games and saves.</p>
      <p class="mt-3">To play Steam games at the same time, use separate Steam accounts and make sure each player has access
        to the game. Steam's account and library-sharing rules still apply.</p>
      <p class="mt-3">This PC’s desktop and apps use its usual desktop session.
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
