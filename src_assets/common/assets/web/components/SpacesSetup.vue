<template>
  <details class="section-card" :open="!setup?.available || !setup?.host_prerequisites_ready || !!error" :aria-busy="loading">
    <summary class="focus-ring cursor-pointer rounded py-2 font-semibold text-silver">
      Host Setup
      <span class="ml-2 text-sm font-normal text-storm">{{ error ? 'Needs Attention' : setup?.available && setup?.host_prerequisites_ready ? 'Configured' : setup?.configured ? 'Needs Attention' : 'Set Up Spaces' }}</span>
    </summary>
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div>
        <h2 id="spaces-setup-title" class="section-title">Check Host</h2>
        <p v-if="!setup?.available" class="mt-2 max-w-2xl text-sm text-storm">
          Polaris runs on your Linux PC. Docker runs the separate gaming spaces on that same PC.
        </p>
      </div>
      <button type="button" class="focus-ring rounded-lg border border-ice/30 px-3 py-2.5 text-sm text-ice disabled:opacity-40"
              :disabled="loading" @click="refresh">{{ loading ? 'Checking…' : 'Recheck Setup' }}</button>
    </div>
    <p v-if="error" class="mt-4 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <p v-else-if="loading" class="mt-4 text-sm text-storm" role="status">Checking Docker, graphics, controls, and host security…</p>
    <template v-if="setup">
      <p class="mt-4 text-sm text-silver" role="status">
        {{ setup.host_prerequisites_ready ? 'Host prerequisites checked.' : 'Complete the steps below, then recheck setup.' }}
        {{ setup.available ? 'Spaces are configured on this host.' : 'Spaces configuration still needs attention.' }}
      </p>
      <details class="mt-4" :open="!setup.host_prerequisites_ready || (setup.configured && !setup.available)">
        <summary class="focus-ring cursor-pointer rounded py-2 text-sm text-ice">Setup Checks</summary>
        <ol class="mt-3 grid gap-3">
          <li v-for="check in setup.checks" :key="check.id" class="min-w-0 rounded-xl border border-storm/20 bg-deep/40 p-4"
              :data-setup-check="check.id">
            <div class="flex flex-wrap items-center justify-between gap-2">
              <h3 class="text-sm font-semibold text-silver">{{ check.title }}</h3>
              <span class="text-xs" :class="check.state === 'ready' ? 'text-success' : 'text-storm'">
                {{ check.state === 'ready' ? 'Checked' : check.state === 'not_configured' ? 'Not configured' : 'Needs attention' }}
              </span>
            </div>
            <p class="mt-2 text-sm text-storm">{{ check.detail }}</p>
            <a v-if="check.state !== 'ready' && ['docker', 'docker_access', 'security'].includes(check.id)"
               :href="'https://github.com/papi-ux/polaris/blob/master/docs/spaces.md#' + (check.id === 'security' ? 'prepare-spaces-security-support' : 'prepare-docker-from-spaces')"
               target="_blank" rel="noopener noreferrer" class="focus-ring mt-3 inline-block rounded py-2 text-sm text-ice hover:underline">
              {{ check.id === 'security' ? 'Security Setup Guide' : 'Docker Setup Guide' }}
            </a>
            <router-link v-if="['input', 'gpu', 'security'].includes(check.id) && check.state !== 'ready'"
                         to="/troubleshooting" class="focus-ring mt-3 inline-block rounded py-2 text-sm text-ice hover:underline">
              Open Doctor &amp; Support
            </router-link>
            <p v-if="check.id === 'spaces' && !setup.configured" class="mt-3 text-sm text-storm">
              After the host checks pass, prepare your first space below.
              The preview will show whether a verified gaming runtime is available for download.
            </p>
          </li>
        </ol>
      </details>
      <p v-if="!setup.available" class="mt-4 text-xs text-storm">These checks do not install packages, restart services, or interrupt games. Game and stream quality are checked when you play.</p>
      <SpacesFirstSetup v-if="!setup.configured" id="spaces-prepare" :host-ready="setup.host_prerequisites_ready" />
    </template>
  </details>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import { validSetup } from '../spaces-setup.js'
import SpacesFirstSetup from './SpacesFirstSetup.vue'

const setup = ref(null), loading = ref(false), error = ref('')
let request
onUnmounted(() => request?.abort())
async function refresh() {
  if (loading.value) return
  loading.value = true; error.value = ''
  request = new AbortController()
  const timeout = setTimeout(() => request.abort(), 12000)
  try {
    const response = await fetch('./api/spaces/setup', { credentials: 'include', cache: 'no-store', signal: request.signal })
    if (!response.ok) throw new Error(response.status === 404
      ? 'This Polaris host does not provide Spaces setup checks. Use a Linux build with Spaces support.'
      : 'Could not check this host. Check your connection and sign in again if needed, then recheck setup.')
    const next = await response.json()
    if (!validSetup(next)) throw new Error('The host setup response could not be verified. Recheck setup before continuing.')
    setup.value = next
  } catch (cause) {
    setup.value = null
    error.value = cause.name === 'AbortError' ? 'The host check timed out. Recheck setup to try again.' :
      cause.message || 'Could not check this host. Recheck setup to try again.'
  } finally { clearTimeout(timeout); loading.value = false }
}
onMounted(refresh)
</script>
