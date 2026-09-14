<template>
  <section class="section-card" aria-labelledby="spaces-setup-title" :aria-busy="loading">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div>
        <h2 id="spaces-setup-title" class="section-title">{{ setup?.available ? 'Host setup' : 'Set up this host' }}</h2>
        <p v-if="!setup?.available" class="mt-2 max-w-2xl text-sm text-storm">
          Polaris runs on your Linux PC. Docker runs the separate gaming spaces on that same PC.
        </p>
      </div>
      <button type="button" class="focus-ring rounded-lg border border-ice/30 px-3 py-2.5 text-sm text-ice disabled:opacity-40"
              :disabled="loading" @click="refresh">{{ loading ? 'Checking…' : 'Recheck setup' }}</button>
    </div>
    <p v-if="error" class="mt-4 text-sm text-warning-bright" role="alert">{{ error }}</p>
    <p v-else-if="loading" class="mt-4 text-sm text-storm" role="status">Checking Docker, graphics, controls, and host security…</p>
    <template v-if="setup">
      <p class="mt-4 text-sm text-silver" role="status">
        {{ setup.host_prerequisites_ready ? 'Host prerequisites checked.' : 'Complete the steps below, then recheck setup.' }}
        {{ setup.available ? 'Spaces are configured on this host.' : 'Spaces configuration still needs attention.' }}
      </p>
      <details class="mt-4" :open="!setup.host_prerequisites_ready || (setup.configured && !setup.available)">
        <summary class="focus-ring cursor-pointer rounded py-2 text-sm text-ice">Docker and host setup</summary>
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
            <details v-if="check.id === 'docker' && check.state !== 'ready'" class="mt-3">
              <summary class="focus-ring cursor-pointer rounded py-1 text-sm text-ice">Install Docker step by step</summary>
              <div class="mt-3 space-y-4 text-sm text-storm">
                <p>Run these commands in a terminal on the Linux PC running Polaris. Your administrator password stays in that terminal.</p>
                <template v-if="guide">
                  <p>{{ guide.name }} installation. If Docker is already installed or your package manager reports a conflict, review the
                    <a :href="guide.url" target="_blank" rel="noopener noreferrer" class="focus-ring text-ice hover:underline">installation guide</a>
                    before replacing packages.</p>
                  <div v-for="(step, index) in guide.steps" :key="step.title">
                    <h4 class="mb-2 font-medium text-silver">{{ index + 1 }}. {{ step.title }}</h4>
                    <pre class="overflow-x-auto rounded-lg bg-void/60 p-3 text-xs text-silver"><code>{{ step.command }}</code></pre>
                    <button type="button" class="focus-ring mt-1 rounded px-1 py-2 text-xs text-ice"
                            :aria-label="'Copy command: ' + step.title" @click="copy(step.command)">Copy command</button>
                  </div>
                </template>
                <p v-else-if="setup.immutable_host || ['bazzite', 'steamos'].includes(setup.distribution)">
                  This host uses a system image. Automatic Docker installation for this system is not available in the Spaces preview.
                  Use your distribution's supported installation method; the regular Fedora and Arch package commands do not apply here.
                </p>
                <p v-else>
                  We do not have installation steps verified for this distribution yet. Follow the
                  <a href="https://docs.docker.com/engine/install/" target="_blank" rel="noopener noreferrer" class="focus-ring text-ice hover:underline">Docker Engine installation guide</a>
                  for your host, then return here to recheck.
                </p>
              </div>
            </details>
            <details v-if="check.id === 'docker_access' && check.state !== 'ready'" class="mt-3">
              <summary class="focus-ring cursor-pointer rounded py-1 text-sm text-ice">Start Docker and grant access</summary>
              <div class="mt-3 space-y-3 text-sm text-storm">
                <p>After installing Docker, start the system Docker service:</p>
                <p v-if="setup.distribution === 'arch' && !setup.immutable_host">
                  If the update installed a new kernel, save your work and restart this host before starting Docker.
                  Signing out alone does not load the new kernel.
                </p>
                <pre class="overflow-x-auto rounded-lg bg-void/60 p-3 text-xs text-silver"><code>{{ startDocker }}</code></pre>
                <button type="button" class="focus-ring rounded px-1 py-2 text-xs text-ice" @click="copy(startDocker)">Copy start command</button>
                <template v-if="accessCommand">
                  <p>Allow the Linux account running Polaris to use Docker. Docker access grants administrator-level control over this host.</p>
                  <pre class="overflow-x-auto rounded-lg bg-void/60 p-3 text-xs text-silver"><code>{{ accessCommand }}</code></pre>
                  <button type="button" class="focus-ring rounded px-1 py-2 text-xs text-ice" @click="copy(accessCommand)">Copy access command</button>
                  <p>Save your work and stop streams before signing out and back in. Start Polaris again, then select Recheck setup.
                    A system service may need an administrator to restart it so it receives the new group membership.</p>
                </template>
                <a href="https://docs.docker.com/engine/install/linux-postinstall/" target="_blank" rel="noopener noreferrer"
                   class="focus-ring inline-block text-ice hover:underline">Docker access instructions</a>
              </div>
            </details>
            <details v-if="check.id === 'security' && check.state !== 'ready' && check.action === 'install_selinux'" class="mt-3">
              <summary class="focus-ring cursor-pointer rounded py-1 text-sm text-ice">Prepare Spaces security support</summary>
              <div class="mt-3 space-y-3 text-sm text-storm">
                <p v-if="setup.immutable_host">Security installation for system image hosts is not available in this preview.</p>
                <template v-else>
                  <p>These one-time host steps let the gaming container use its reserved controller devices while keeping SELinux enforcing.</p>
                  <template v-if="setup.distribution === 'fedora'">
                    <p>1. In a terminal on the Polaris host, install the policy build tools:</p>
                    <pre class="overflow-x-auto rounded-lg bg-void/60 p-3 text-xs text-silver"><code>{{ fedoraSecurityPackages }}</code></pre>
                    <button type="button" class="focus-ring rounded px-1 py-2 text-xs text-ice" @click="copy(fedoraSecurityPackages)">Copy policy tools command</button>
                  </template>
                  <p v-else>Install your distribution's SELinux development tools and container reference policy first. The helper checks for these files before making changes.</p>
                  <p>2. Finish your games, stop Spaces streams, and quit Polaris. If it runs as a service, stop that service first.</p>
                  <p>3. Run the packaged setup helper in the same host terminal. Your administrator password stays there:</p>
                  <pre class="overflow-x-auto rounded-lg bg-void/60 p-3 text-xs text-silver"><code>{{ installSpacesSecurity }}</code></pre>
                  <button type="button" class="focus-ring rounded px-1 py-2 text-xs text-ice" @click="copy(installSpacesSecurity)">Copy security setup command</button>
                  <p>4. Reopen Polaris, return to Spaces, and select Recheck setup.</p>
                  <p>If setup is interrupted, run the same command again. Existing manually installed policies need review before the helper can manage them.</p>
                </template>
              </div>
            </details>
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
      <SpacesFirstSetup v-if="!setup.configured" :host-ready="setup.host_prerequisites_ready" />
    </template>
    <p v-if="copyStatus" class="mt-3 text-sm text-silver" role="status">{{ copyStatus }}</p>
  </section>
</template>

<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { dockerAccessCommand, installGuide, startDocker, validSetup, installSpacesSecurity, fedoraSecurityPackages } from '../spaces-setup.js'
import SpacesFirstSetup from './SpacesFirstSetup.vue'

const setup = ref(null), loading = ref(false), error = ref(''), copyStatus = ref('')
const guide = computed(() => installGuide(setup.value))
const accessCommand = computed(() => dockerAccessCommand(setup.value?.service_uid))
let request
onUnmounted(() => request?.abort())
async function refresh() {
  if (loading.value) return
  loading.value = true; error.value = ''; setup.value = null
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
    error.value = cause.name === 'AbortError' ? 'The host check timed out. Recheck setup to try again.' :
      cause.message || 'Could not check this host. Recheck setup to try again.'
  } finally { clearTimeout(timeout); loading.value = false }
}
async function copy(command) {
  try { await navigator.clipboard.writeText(command); copyStatus.value = 'Command copied. Run it in the host terminal.' }
  catch { copyStatus.value = 'Could not copy. Select the command text and copy it from here.' }
}
onMounted(refresh)
</script>
