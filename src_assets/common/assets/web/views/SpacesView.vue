<template>
  <div class="page-shell operator-console pb-2">
    <section class="page-header">
      <div class="page-heading">
        <p class="section-kicker">{{ $t('spaces.kicker') }}</p>
        <h1 class="page-title">{{ $t('spaces.title') }}</h1>
        <p class="page-subtitle">{{ $t('spaces.subtitle') }}</p>
      </div>
      <div class="page-meta page-actions">
        <div class="page-actions-primary">
          <a :href="docsUrl" target="_blank" rel="noopener noreferrer"
             class="focus-ring inline-flex h-9 items-center rounded-lg border border-storm px-4 text-sm font-medium text-silver transition-colors hover:border-ice hover:text-ice">
            {{ $t('spaces.guide') }}
          </a>
          <span class="meta-pill" :title="$t('spaces.preview_limits')">{{ $t('spaces.preview') }}</span>
        </div>
      </div>
    </section>

    <section v-if="firstSpaceMissing" class="section-card" aria-labelledby="spaces-intro-title">
      <h2 id="spaces-intro-title" class="section-title">{{ runtimeWaiting ? $t('spaces.intro_waiting_title') : $t('spaces.intro_title') }}</h2>
      <p class="mt-2 text-sm text-storm">{{ runtimeWaiting ? $t('spaces.intro_waiting_copy') : $t('spaces.intro_copy') }}</p>
      <a v-if="runtimeWaiting" href="https://papi-ux.com/docs/spaces-or-regular/" target="_blank" rel="noopener noreferrer"
         data-spaces-or-regular class="mt-3 inline-flex text-sm font-medium text-ice underline-offset-4 hover:underline">
        {{ $t('spaces.intro_waiting_link') }}
      </a>
    </section>

    <p v-if="clientLoading" class="text-sm text-storm" role="status">{{ $t('spaces.devices_loading') }}</p>
    <div v-if="clientError" class="flex flex-wrap items-center gap-3 text-sm text-warning-bright" role="alert">
      <span>{{ clientError }}</span>
      <Button variant="outline" size="sm" data-clients-retry @click="loadClients">{{ $t('spaces.devices_retry') }}</Button>
    </div>

    <div class="flex flex-col gap-6">
      <MultiseatAssignments :class="hostFirst ? 'order-2' : 'order-1'" :clients="clients"
                            :clients-ready="!clientLoading && !clientError" @snapshot="snapshot = $event" />
      <SpacesSetup :class="hostFirst ? 'order-1' : 'order-2'" @state="setupState = $event" @runtime-waiting="runtimeWaiting = $event" />
    </div>

    <div class="flex flex-wrap items-center justify-between gap-3 text-sm text-storm">
      <span>{{ $t('spaces.devices_footer') }}</span>
      <router-link to="/pin" class="focus-ring rounded px-1 py-2 text-ice hover:underline">{{ $t('spaces.devices_open') }}</router-link>
    </div>
  </div>
</template>

<script setup>
import { computed, inject, onMounted, onUnmounted, ref } from 'vue'
import Button from '../components/Button.vue'
import SpacesSetup from '../components/SpacesSetup.vue'
import MultiseatAssignments from '../components/MultiseatAssignments.vue'
import { docsUrl } from '../spaces-setup.js'

const i18n = inject('i18n')
const clients = ref([]), snapshot = ref(null), setupState = ref(null), clientError = ref('')
const clientLoading = ref(true)
// True while this build has no verified gaming runtime: no Space can be created
// yet, so the intro speaks about getting the PC ready instead.
const runtimeWaiting = ref(false)
let request = null
onUnmounted(() => request?.abort())

// The intro card only once a snapshot has been read and shows no live Space;
// a failed or pending load says nothing about how many Spaces exist.
const firstSpaceMissing = computed(() => Array.isArray(snapshot.value?.profiles) &&
  !snapshot.value.profiles.some(space => !space.archived))
// Host Setup leads until the host can offer Spaces; a configured host keeps
// its Spaces first and the checks collapsed below them.
const hostFirst = computed(() => !setupState.value || !setupState.value.available)

async function loadClients() {
  request?.abort()
  const current = new AbortController()
  request = current
  clientLoading.value = true
  clientError.value = ''
  const timeout = setTimeout(() => current.abort(), 12000)
  try {
    const response = await fetch('./api/clients/list', { credentials: 'include', cache: 'no-store', signal: current.signal })
    const next = await response.json()
    if (!response.ok || next?.status !== true || !Array.isArray(next.named_certs) ||
        !next.named_certs.every(client => client && typeof client.uuid === 'string' && client.uuid)) throw new Error()
    if (request !== current) return
    clients.value = next.named_certs
  } catch {
    if (request !== current) return
    clientError.value = i18n.t('spaces.devices_error')
  } finally {
    clearTimeout(timeout)
    if (request === current) clientLoading.value = false
  }
}
onMounted(loadClients)
</script>
