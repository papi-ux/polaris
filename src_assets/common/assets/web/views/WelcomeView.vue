<template>
  <div class="relative mx-auto flex min-h-screen max-w-7xl items-center justify-center px-4 py-8 sm:px-6">
    <div class="w-full max-w-5xl">
      <section class="page-hero auth-stage auth-stage--welcome overflow-visible">
        <div class="page-hero-content gap-6 xl:grid-cols-1">
          <div class="page-hero-copy">
            <div class="flex items-center gap-3">
              <img src="/images/logo-polaris-45.png" class="h-11" alt="Polaris">
              <div>
                <div class="page-hero-kicker">First-Time Setup</div>
                <h1 class="page-hero-title">{{ $t('welcome.greeting') }}</h1>
              </div>
            </div>

            <p class="page-hero-copy-text max-w-2xl">
              Secure the host, check the runtime, then publish an app and pair a client.
            </p>
          </div>
        </div>

        <div class="relative z-[1] border-t border-storm/15 px-6 py-6 md:px-7 md:py-7">
          <div class="flex flex-wrap gap-2">
            <div
              v-for="(stepDef, idx) in steps"
              :key="stepDef.title"
              class="auth-step-pill"
              :class="{
                'is-active': idx === currentStep,
                'is-complete': idx < currentStep
              }"
            >
              <span class="font-medium">{{ idx + 1 }}.</span>
              <span class="ml-2">{{ stepDef.title }}</span>
            </div>
          </div>

          <div class="auth-work-surface mt-6">
            <div class="mb-5 flex flex-col gap-3 border-b border-storm/15 pb-5 sm:flex-row sm:items-end sm:justify-between">
              <div>
                <div class="text-[10px] font-semibold uppercase tracking-[0.22em] text-storm">Step {{ currentStep + 1 }} of {{ steps.length }}</div>
                <div class="mt-2 flex items-center gap-2">
                  <h2 class="text-2xl font-semibold text-silver">{{ steps[currentStep].title }}</h2>
                  <InfoHint :label="`${steps[currentStep].title} guidance`">
                    {{ steps[currentStep].hint }}
                  </InfoHint>
                </div>
                <p class="mt-2 text-sm text-storm">{{ steps[currentStep].summary }}</p>
              </div>
            </div>

            <div v-if="currentStep === 0">
              <div v-if="credentialsAlreadyConfigured" class="space-y-4">
                <div class="flex items-start justify-between gap-3 rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  <div class="font-medium">This host already has web credentials and this browser is signed in.</div>
                  <InfoHint align="right" label="Credential reuse">
                    Continue to review runtime defaults, or open Password Settings if you want to rotate the existing credentials.
                  </InfoHint>
                </div>
                <a href="#/password" class="inline-flex h-10 items-center justify-center rounded-xl border border-storm/25 bg-deep/50 px-4 text-sm font-medium text-ice transition-[background-color,border-color,color] duration-200 hover:border-ice/30 hover:bg-twilight/35 no-underline">
                  Open Password Settings
                </a>
              </div>
              <div v-else-if="requiresLoginForExistingCredentials" class="space-y-4">
                <div class="flex items-start justify-between gap-3 rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                  <div class="font-medium">This host already has web credentials. Sign in first to revisit the rest of setup.</div>
                  <InfoHint align="right" label="Wizard access">
                    The GPU, network, and pairing steps need an authenticated session because they read the active host configuration.
                  </InfoHint>
                </div>
                <a href="#/login?redirect=/welcome" class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)] no-underline">
                  Sign In To Continue
                </a>
              </div>
              <form v-else @submit.prevent="save" class="space-y-4">
                <div class="mb-4 flex items-start justify-between gap-3 rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                  <div class="font-medium">Store this account somewhere safe.</div>
                  <InfoHint align="right" label="Credential safety">
                    Polaris will not surface the password again after setup. Keep the web UI credentials in a password manager before you continue.
                  </InfoHint>
                </div>
                <div>
                  <label for="usernameInput" class="mb-1 block text-sm font-medium text-storm">{{ $t('_common.username') }}</label>
                  <input type="text" class="w-full rounded-xl border border-storm/30 bg-void/55 px-3 py-3 text-silver transition-[border-color,background-color,box-shadow] duration-200 focus:border-ice/40 focus:bg-void/70 focus:outline-none focus:shadow-[0_0_20px_rgba(200,214,229,0.08)]" id="usernameInput" autocomplete="username"
                    v-model="passwordData.newUsername" />
                </div>
                <div>
                  <label for="passwordInput" class="mb-1 block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
                  <input type="password" class="w-full rounded-xl border border-storm/30 bg-void/55 px-3 py-3 text-silver transition-[border-color,background-color,box-shadow] duration-200 focus:border-ice/40 focus:bg-void/70 focus:outline-none focus:shadow-[0_0_20px_rgba(200,214,229,0.08)]" id="passwordInput" autocomplete="new-password"
                    v-model="passwordData.newPassword" required />
                </div>
                <div>
                  <label for="confirmPasswordInput" class="mb-1 block text-sm font-medium text-storm">{{ $t('welcome.confirm_password') }}</label>
                  <input type="password" class="w-full rounded-xl border border-storm/30 bg-void/55 px-3 py-3 text-silver transition-[border-color,background-color,box-shadow] duration-200 focus:border-ice/40 focus:bg-void/70 focus:outline-none focus:shadow-[0_0_20px_rgba(200,214,229,0.08)]" id="confirmPasswordInput" autocomplete="new-password"
                    v-model="passwordData.confirmNewPassword" required />
                </div>
                <button type="submit" class="inline-flex h-11 w-full items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)] disabled:opacity-50" v-bind:disabled="loading">
                  {{ loading ? 'Saving…' : 'Save Credentials' }}
                </button>
                <div class="rounded-2xl border border-red-500/25 bg-red-500/10 px-4 py-3 text-sm text-red-100" v-if="error"><b>{{ $t('_common.error') }}</b> {{error}}</div>
                <div class="rounded-2xl border border-green-500/25 bg-green-500/10 px-4 py-3 text-sm text-green-100" v-if="success">
                  <b>{{ $t('_common.success') }}</b> Credentials saved. Continue when you are ready.
                </div>
              </form>
            </div>

            <div v-if="currentStep === 1">
              <div v-if="runtimeLoading" class="rounded-2xl border border-storm/20 bg-void/45 px-4 py-8 text-center text-sm text-storm">Loading runtime telemetry…</div>
              <div v-else-if="runtimeData" class="space-y-3">
                <div v-if="runtimeModeLabel" class="surface-subtle p-4">
                  <div class="text-sm text-storm">Capture Mode</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ runtimeModeLabel }}</div>
                  <div v-if="runtimeModeDetail" class="mt-1 text-sm text-storm">{{ runtimeModeDetail }}</div>
                </div>
                <div class="surface-subtle p-4">
                  <div class="text-sm text-storm">Video Encoder</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ detectedEncoder }}</div>
                </div>
                <div v-if="detectedAdapter" class="surface-subtle p-4">
                  <div class="text-sm text-storm">{{ detectedAdapterLabel }}</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ detectedAdapter }}</div>
                </div>
                <div class="flex items-center justify-between gap-3 rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  <span>Looks good if the encoder and adapter match your host.</span>
                  <InfoHint align="right" label="Runtime defaults">
                    You can fine-tune encoder and display behavior later in Configuration → Audio/Video. This step is only a quick sanity check.
                  </InfoHint>
                </div>
              </div>
              <div v-else class="rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                Could not load live hardware detection. You can still continue and revisit hardware settings later.
              </div>
            </div>

            <div v-if="currentStep === 2">
              <div v-if="configLoading" class="rounded-2xl border border-storm/20 bg-void/45 px-4 py-8 text-center text-sm text-storm">Loading network settings…</div>
              <div v-else class="space-y-3">
                <div class="surface-subtle p-4">
                  <div class="text-sm text-storm">Streaming Ports</div>
                  <div class="mt-2 text-base font-medium text-silver">
                    TCP: {{ httpsPort }} HTTPS, {{ streamingPort }} HTTP, {{ rtspPort }} RTSP
                  </div>
                  <div class="mt-1 text-base font-medium text-silver">
                    UDP: {{ udpPortRange }}, {{ udpAuxPorts }}
                  </div>
                </div>
                <div class="flex items-center justify-between gap-3 rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  <span>{{ networkExposureSummary }}</span>
                  <InfoHint align="right" label="Network path guidance">
                    Keep host firewall rules and any router forwarding aligned with these derived ports before the first client test.
                  </InfoHint>
                </div>
                <div class="rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  You can change the host port later in Configuration → Network.
                </div>
              </div>
            </div>

            <div v-if="currentStep === 3">
              <div class="space-y-3">
                <div class="surface-subtle p-4">
                  <div class="text-sm text-storm">Published Apps</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ firstAppStatusTitle }}</div>
                  <div class="mt-1 text-sm text-storm">{{ firstAppStatusDetail }}</div>
                </div>
                <div class="surface-subtle p-4">
                  <div class="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
                    <div>
                      <div class="text-sm text-storm">Launcher Import</div>
                      <div class="mt-2 text-base font-medium text-silver">{{ launcherScanStatusTitle }}</div>
                      <div class="mt-1 text-sm text-storm">{{ launcherScanStatusDetail }}</div>
                    </div>
                    <button
                      type="button"
                      @click="scanLauncherSources"
                      :disabled="launcherScanLoading"
                      class="inline-flex h-10 items-center justify-center rounded-xl border border-storm/25 bg-deep/50 px-4 text-sm font-medium text-ice transition-[background-color,border-color,color] duration-200 hover:border-ice/30 hover:bg-twilight/35 disabled:cursor-not-allowed disabled:opacity-50"
                    >
                      {{ launcherScanLoading ? 'Scanning…' : launcherScanAttempted ? 'Rescan Launchers' : 'Scan Installed Libraries' }}
                    </button>
                  </div>
                  <div v-if="launcherSources.length" class="mt-3 flex flex-wrap gap-2">
                    <span
                      v-for="source in launcherSources"
                      :key="source.key"
                      class="inline-flex items-center rounded-full border px-3 py-1 text-xs font-medium"
                      :class="source.className"
                    >
                      {{ source.label }} {{ source.ready }} ready
                    </span>
                  </div>
                </div>
                <div class="rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  {{ firstAppGuidance }}
                </div>
                <div class="flex flex-wrap gap-3">
                  <router-link
                    :to="primaryAppCta.to"
                    class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)] no-underline"
                  >
                    {{ primaryAppCta.label }}
                  </router-link>
                  <router-link
                    :to="secondaryAppCta.to"
                    class="inline-flex h-10 items-center justify-center rounded-xl border border-storm/25 bg-deep/50 px-4 text-sm font-medium text-ice transition-[background-color,border-color,color] duration-200 hover:border-ice/30 hover:bg-twilight/35 no-underline"
                  >
                    {{ secondaryAppCta.label }}
                  </router-link>
                </div>
              </div>
            </div>

            <div v-if="currentStep === 4">
              <div class="space-y-3">
                <div class="surface-subtle p-4">
                  <div class="text-sm text-storm">Saved Devices</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ pairingStatusTitle }}</div>
                  <div class="mt-1 text-sm text-storm">{{ pairingStatusDetail }}</div>
                </div>
                <div class="surface-subtle p-4">
                  <div class="text-sm text-storm">Recommended Pairing Route</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ pairingRouteTitle }}</div>
                  <div class="mt-1 text-sm text-storm">{{ pairingRouteDetail }}</div>
                </div>
                <div class="surface-subtle p-4">
                  <div class="text-sm text-storm">Trusted Network</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ trustedPairingTitle }}</div>
                  <div class="mt-1 text-sm text-storm">{{ trustedPairingDetail }}</div>
                </div>
                <div class="rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  {{ pairingGuidance }}
                </div>
                <div class="flex flex-wrap gap-3">
                  <a
                    :href="primaryPairingCta.href"
                    class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)] no-underline"
                  >
                    {{ primaryPairingCta.label }}
                  </a>
                  <a
                    :href="secondaryPairingCta.href"
                    class="inline-flex h-10 items-center justify-center rounded-xl border border-storm/25 bg-deep/50 px-4 text-sm font-medium text-ice transition-[background-color,border-color,color] duration-200 hover:border-ice/30 hover:bg-twilight/35 no-underline"
                  >
                    {{ secondaryPairingCta.label }}
                  </a>
                </div>
              </div>
            </div>
          </div>

          <div class="mt-6 flex justify-between gap-3">
            <button
              v-if="currentStep > 0"
              @click="currentStep--"
              class="inline-flex h-10 items-center justify-center rounded-xl border border-storm/25 bg-deep/40 px-4 text-sm font-medium text-silver transition-[background-color,border-color,color] duration-200 hover:border-storm/40 hover:bg-twilight/35"
            >
              Back
            </button>
            <div v-else></div>
            <button
              v-if="currentStep < steps.length - 1"
              @click="nextStep"
              :disabled="currentStep === 0 && !success"
              class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)] disabled:cursor-not-allowed disabled:opacity-50"
            >
              Next
            </button>
            <button
              v-else
              @click="finishWizard"
              class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)]"
            >
              Finish Setup
            </button>
          </div>
        </div>
      </section>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import InfoHint from '../components/InfoHint.vue'
import { useGameScanner } from '../composables/useGameScanner.js'

const currentStep = ref(0)
const error = ref(null)
const success = ref(false)
const loading = ref(false)
const configLoading = ref(false)
const configData = ref(null)
const runtimeLoading = ref(false)
const runtimeData = ref(null)
const appsLoading = ref(false)
const appsData = ref(null)
const pairingLoading = ref(false)
const pairingData = ref(null)
const credentialsAlreadyConfigured = ref(false)
const requiresLoginForExistingCredentials = ref(false)
const launcherScanAttempted = ref(false)

const {
  scanning: launcherScanLoading,
  steamGames,
  lutrisGames,
  heroicGames,
  error: launcherScanError,
  scan: scanGames,
} = useGameScanner()

const passwordData = reactive({
  newUsername: "polaris",
  newPassword: "",
  confirmNewPassword: "",
})

const steps = [
  {
    title: 'Credentials',
    summary: 'Create the local web console account before exposing any control surface.',
    hint: 'These credentials protect the Polaris web UI on this host. Save them before continuing because Polaris will not display the password again later.',
    tags: ['Required', 'Security'],
  },
  {
    title: 'GPU Detection',
    summary: 'Review the detected encoder and adapter so the first stream starts from sane defaults.',
    hint: 'This is a quick confidence check, not a full tuning pass. If the hardware looks wrong, you can correct it later in Configuration.',
    tags: ['Runtime', 'Encoder'],
  },
  {
    title: 'Network',
    summary: 'Confirm the host ports and firewall path before you invite the first client.',
    hint: 'Start with LAN-only streaming. Remote exposure and port forwarding can wait until the host is stable and tested.',
    tags: ['Ports', 'Firewall'],
  },
  {
    title: 'First App',
    summary: 'Publish the apps you actually want clients to see instead of dumping everything into the launcher.',
    hint: 'A smaller, intentional library is easier to maintain. You can add scans, box art, and per-app tuning after the first entries exist.',
    tags: ['Library', 'Curation'],
  },
  {
    title: 'Pair Client',
    summary: 'Finish by pairing a client and launching a real session from the host you just prepared.',
    hint: 'Nova and Moonlight both work. Pick the client that fits the device, then approve it from the Polaris pairing surface.',
    tags: ['Clients', 'Launch'],
  },
]

const defaultStreamingPort = 47989

function numericConfigValue(value, fallback) {
  const parsed = Number.parseInt(value, 10)
  return Number.isFinite(parsed) ? parsed : fallback
}

function nextStep() {
  if (currentStep.value === 0 && !success.value) return
  currentStep.value++
  if (currentStep.value === 1 && !runtimeData.value) {
    loadRuntime()
  } else if (currentStep.value === 2 && !configData.value) {
    loadConfig()
  } else if (currentStep.value === 3 && !appsData.value) {
    loadAppsSummary()
  } else if (currentStep.value === 4) {
    if (!configData.value) {
      loadConfig()
    }
    if (!pairingData.value) {
      loadPairingSummary()
    }
  }
}

async function detectWizardState() {
  try {
    const res = await fetch('./api/config', { credentials: 'include' })
    const finalPath = res.redirected ? new URL(res.url).pathname : ''
    if (finalPath.endsWith('/welcome')) {
      return
    }
    if (finalPath.endsWith('/login') || res.status === 401) {
      requiresLoginForExistingCredentials.value = true
      return
    }
    if (!res.ok) {
      return
    }

    const contentType = res.headers.get('content-type') || ''
    if (!contentType.includes('application/json')) {
      return
    }

    configData.value = await res.json()
    credentialsAlreadyConfigured.value = true
    success.value = true
  } catch (e) {
    console.error('Failed to detect wizard state', e)
  }
}

async function loadConfig() {
  configLoading.value = true
  try {
    const res = await fetch('./api/config', { credentials: 'include' })
    if (res.ok) {
      configData.value = await res.json()
    }
  } catch (e) {
    console.error('Failed to load config', e)
  } finally {
    configLoading.value = false
  }
}

async function loadRuntime() {
  runtimeLoading.value = true
  try {
    const res = await fetch('./api/stats/system', { credentials: 'include' })
    if (res.ok) {
      runtimeData.value = await res.json()
    }
  } catch (e) {
    console.error('Failed to load runtime telemetry', e)
  } finally {
    runtimeLoading.value = false
  }
}

async function loadAppsSummary() {
  appsLoading.value = true
  try {
    const res = await fetch('./api/apps', { credentials: 'include' })
    if (res.ok) {
      appsData.value = await res.json()
    }
  } catch (e) {
    console.error('Failed to load app catalog', e)
  } finally {
    appsLoading.value = false
  }
}

async function loadPairingSummary() {
  pairingLoading.value = true
  try {
    const res = await fetch('./api/clients/list', { credentials: 'include' })
    if (res.ok) {
      pairingData.value = await res.json()
    }
  } catch (e) {
    console.error('Failed to load pairing summary', e)
  } finally {
    pairingLoading.value = false
  }
}

async function scanLauncherSources() {
  launcherScanAttempted.value = true
  await scanGames()
}

function preferredDisplay() {
  const displays = runtimeData.value?.displays || []
  return displays.find((display) => display.primary) || displays[0] || null
}

function formatDisplayLabel(display) {
  if (!display) return ''
  return display.label || display.friendly_name || display.name || display.device_id || ''
}

function joinDisplayParts(parts) {
  return parts.filter(Boolean).join(' • ')
}

function configFlag(value) {
  if (typeof value === 'string') {
    return ['enabled', 'true', '1', 'yes', 'on'].includes(value.trim().toLowerCase())
  }
  return Boolean(value)
}

function parseTrustedSubnets(value) {
  if (Array.isArray(value)) {
    return value.map((entry) => String(entry).trim()).filter(Boolean)
  }
  if (typeof value !== 'string' || !value.trim()) {
    return []
  }

  try {
    const parsed = JSON.parse(value)
    if (Array.isArray(parsed)) {
      return parsed.map((entry) => String(entry).trim()).filter(Boolean)
    }
  } catch {
    return value.split(/[\n,]+/).map((entry) => entry.trim()).filter(Boolean)
  }

  return []
}

const detectedEncoder = computed(() => {
  return runtimeData.value?.video?.active_encoder || 'Auto-detected'
})

const streamingPort = computed(() => numericConfigValue(configData.value?.port, defaultStreamingPort))
const httpsPort = computed(() => streamingPort.value - 5)
const rtspPort = computed(() => streamingPort.value + 21)
const udpPortRange = computed(() => `${streamingPort.value + 9}-${streamingPort.value + 11}`)
const udpAuxPorts = computed(() => `${streamingPort.value + 13}, ${streamingPort.value + 21}`)
const networkExposureSummary = computed(() => {
  if (configFlag(configData.value?.upnp)) {
    return 'UPnP is enabled, but router and firewall behavior should still be verified from the client network.'
  }
  return 'Host firewall and router rules still need to allow the ports above before remote or strict-LAN clients can connect.'
})

const detectedDisplay = computed(() => {
  return formatDisplayLabel(preferredDisplay())
})

const detectedAdapterLabel = computed(() => {
  return detectedDisplay.value ? 'GPU / Display Adapter' : 'GPU Adapter'
})

const detectedAdapter = computed(() => {
  const gpuName = runtimeData.value?.gpu?.name || ''
  return joinDisplayParts([gpuName, detectedDisplay.value])
})

const runtimeModeLabel = computed(() => {
  const effectiveHeadless = Boolean(runtimeData.value?.runtime?.effective_headless)
  if (effectiveHeadless) {
    return 'Headless streaming'
  }
  if (runtimeData.value?.runtime?.backend) {
    return 'Desktop-attached streaming'
  }
  return ''
})

const runtimeModeDetail = computed(() => {
  const backend = runtimeData.value?.runtime?.backend || ''
  const effectiveHeadless = Boolean(runtimeData.value?.runtime?.effective_headless)
  if (effectiveHeadless) {
    return backend
      ? `${backend} is currently running without a pinned physical output.`
      : 'This runtime is currently running without a pinned physical output.'
  }
  if (!detectedDisplay.value) {
    return 'A physical output was not surfaced by the runtime telemetry yet.'
  }
  return ''
})

const publishedApps = computed(() => {
  const apps = appsData.value?.apps
  if (!Array.isArray(apps)) {
    return []
  }
  return apps.filter((app) => app?.uuid)
})

const publishedAppCount = computed(() => {
  return publishedApps.value.length
})

const currentPublishedApp = computed(() => {
  const currentAppUuid = appsData.value?.current_app || ''
  return publishedApps.value.find((app) => app.uuid === currentAppUuid) || null
})

const launcherSources = computed(() => {
  return [
    {
      key: 'steam',
      label: 'Steam',
      ready: steamGames.value.filter((game) => !game.already_imported).length,
      total: steamGames.value.length,
      className: 'border-blue-400/25 bg-blue-500/10 text-blue-100',
    },
    {
      key: 'lutris',
      label: 'Lutris',
      ready: lutrisGames.value.filter((game) => !game.already_imported).length,
      total: lutrisGames.value.length,
      className: 'border-orange-400/25 bg-orange-500/10 text-orange-200',
    },
    {
      key: 'heroic',
      label: 'Heroic',
      ready: heroicGames.value.filter((game) => !game.already_imported).length,
      total: heroicGames.value.length,
      className: 'border-purple-400/25 bg-purple-500/10 text-purple-100',
    },
  ].filter((source) => source.total > 0)
})

const launcherReadyCount = computed(() => {
  return launcherSources.value.reduce((total, source) => total + source.ready, 0)
})

const firstAppStatusTitle = computed(() => {
  if (appsLoading.value) {
    return 'Checking published library…'
  }
  if (publishedAppCount.value > 0) {
    return `${publishedAppCount.value} app${publishedAppCount.value === 1 ? '' : 's'} already published`
  }
  return 'No applications published yet'
})

const firstAppStatusDetail = computed(() => {
  if (appsLoading.value) {
    return 'Polaris is loading the current library state for this host.'
  }
  if (currentPublishedApp.value) {
    return `Current live title: ${currentPublishedApp.value.name}.`
  }
  if (publishedAppCount.value > 0) {
    return 'Your library is already populated enough for a first client test.'
  }
  return 'The quickest first win is importing an installed title or creating one manual launch entry.'
})

const launcherScanStatusTitle = computed(() => {
  if (launcherScanLoading.value) {
    return 'Scanning installed launchers…'
  }
  if (launcherSources.value.length > 0) {
    if (launcherReadyCount.value > 0) {
      return `${launcherReadyCount.value} import-ready title${launcherReadyCount.value === 1 ? '' : 's'} detected`
    }
    return 'Supported launchers found, but everything detected is already published'
  }
  if (launcherScanError.value) {
    return 'Launcher scan is unavailable right now'
  }
  if (launcherScanAttempted.value) {
    return 'No supported launcher libraries were detected'
  }
  return 'Launcher scan has not been run yet'
})

const launcherScanStatusDetail = computed(() => {
  if (launcherScanLoading.value) {
    return 'Polaris is checking Steam, Lutris, and Heroic for import candidates.'
  }
  if (launcherSources.value.length > 0) {
    const labels = launcherSources.value.map((source) => source.label).join(', ')
    if (launcherReadyCount.value > 0) {
      return `Open the import workspace to stage titles from ${labels}.`
    }
    return `Polaris checked ${labels}, and the detected titles are already in the published library.`
  }
  if (launcherScanError.value) {
    return `${launcherScanError.value} You can still create a blank app manually.`
  }
  if (launcherScanAttempted.value) {
    return 'Create a blank app manually, or come back later after installing a supported launcher.'
  }
  return 'Run a scan if you want Polaris to look for existing launcher libraries before you create a manual entry.'
})

const firstAppGuidance = computed(() => {
  if (publishedAppCount.value > 0) {
    return 'One published title is enough to finish setup. You can expand and polish the library after the first client test.'
  }
  if (launcherReadyCount.value > 0) {
    return 'Launcher import is usually the fastest path to a real first session on this host.'
  }
  return 'Manual app creation is the fallback path when Polaris does not detect an importable launcher library.'
})

const primaryAppCta = computed(() => {
  if (publishedAppCount.value > 0) {
    return {
      label: 'Review Published Apps',
      to: { path: '/apps' },
    }
  }

  if (launcherReadyCount.value > 0) {
    return {
      label: 'Open Import Workspace',
      to: { path: '/apps', query: { import: '1', scan: '1' } },
    }
  }

  return {
    label: 'Create First App',
    to: { path: '/apps', query: { new: '1' } },
  }
})

const secondaryAppCta = computed(() => {
  if (publishedAppCount.value > 0) {
    if (launcherReadyCount.value > 0) {
      return {
        label: 'Import More Titles',
        to: { path: '/apps', query: { import: '1', scan: '1' } },
      }
    }

    return {
      label: 'Add Another App',
      to: { path: '/apps', query: { new: '1' } },
    }
  }

  if (launcherReadyCount.value > 0) {
    return {
      label: 'Create Blank App',
      to: { path: '/apps', query: { new: '1' } },
    }
  }

  return {
    label: 'Open Library',
    to: { path: '/apps' },
  }
})

const pairedClients = computed(() => {
  const namedCerts = pairingData.value?.named_certs
  return Array.isArray(namedCerts) ? namedCerts : []
})

const pairedClientCount = computed(() => {
  return pairedClients.value.length
})

const connectedClientCount = computed(() => {
  return pairedClients.value.filter((client) => client.connected).length
})

const pairedClientHighlights = computed(() => {
  return pairedClients.value
    .map((client) => client.friendly_name || client.name || client.uuid)
    .filter(Boolean)
    .slice(0, 2)
})

const pairingEnabled = computed(() => {
  if (!configData.value) {
    return true
  }
  return configFlag(configData.value.enable_pairing ?? true)
})

const trustedSubnets = computed(() => {
  return parseTrustedSubnets(configData.value?.trusted_subnets)
})

const trustedSubnetCount = computed(() => {
  return trustedSubnets.value.length
})

const trustedPairingEnabled = computed(() => {
  return configFlag(configData.value?.trusted_subnet_auto_pairing)
})

const pairingStatusTitle = computed(() => {
  if (pairingLoading.value) {
    return 'Checking saved devices…'
  }
  if (pairedClientCount.value > 0) {
    return `${pairedClientCount.value} device${pairedClientCount.value === 1 ? '' : 's'} already paired`
  }
  return 'No clients paired yet'
})

const pairingStatusDetail = computed(() => {
  if (pairingLoading.value) {
    return 'Polaris is loading the current client roster for this host.'
  }
  if (pairedClientCount.value === 0) {
    return 'Start with one trusted client so you can validate launch, input, and display behavior.'
  }

  const highlights = pairedClientHighlights.value.join(', ')
  if (connectedClientCount.value > 0) {
    return highlights
      ? `${connectedClientCount.value} currently connected. Saved devices include ${highlights}.`
      : `${connectedClientCount.value} paired device${connectedClientCount.value === 1 ? ' is' : 's are'} currently connected.`
  }

  return highlights
    ? `Saved devices include ${highlights}. You can review access or pair another client from the pairing page.`
    : 'Your first client is already trusted. You can review access or pair another device from the pairing page.'
})

const pairingRouteTitle = computed(() => {
  if (!pairingEnabled.value) {
    return 'Pairing is currently disabled'
  }
  if (pairedClientCount.value > 0) {
    return 'Review existing devices or pair another client'
  }
  return 'Start with Nova QR pairing'
})

const pairingRouteDetail = computed(() => {
  if (!pairingEnabled.value) {
    return 'Enable pairing in Network settings before sharing this host with a client.'
  }
  if (pairedClientCount.value > 0) {
    return 'Open the pairing page to inspect saved access, regenerate a QR flow, or fall back to manual PIN pairing.'
  }
  return 'QR pairing is the fastest first-run path for Nova. Manual PIN is still available when you need explicit approval.'
})

const trustedPairingTitle = computed(() => {
  if (!pairingEnabled.value) {
    return 'Enable pairing before trusted-network setup matters'
  }
  if (trustedPairingEnabled.value && trustedSubnetCount.value > 0) {
    return `${trustedSubnetCount.value} trusted subnet${trustedSubnetCount.value === 1 ? '' : 's'} configured for auto-pairing`
  }
  if (trustedSubnetCount.value > 0) {
    return 'Trusted subnets are saved, but auto-pairing is off'
  }
  return 'Trusted network pairing is not configured'
})

const trustedPairingDetail = computed(() => {
  if (!pairingEnabled.value) {
    return 'Turn pairing back on first, then decide whether trusted subnet auto-pairing belongs on this host.'
  }
  if (trustedPairingEnabled.value && trustedSubnetCount.value > 0) {
    return 'Use this only on networks you fully control. Clients on those subnets can take the faster trusted-pair path.'
  }
  if (trustedSubnetCount.value > 0) {
    return 'The subnet list exists, but Polaris will still require manual approval until trusted auto-pairing is enabled.'
  }
  return 'Keep using QR or PIN unless you specifically want a faster trusted-LAN workflow.'
})

const pairingGuidance = computed(() => {
  if (!pairingEnabled.value) {
    return 'Pairing is disabled right now, so the next move is Network settings rather than the client page.'
  }
  if (pairedClientCount.value > 0) {
    return 'One trusted client is enough to finish setup. The next useful test is launching a real app from that device.'
  }
  return 'Use one pairing route, save the device, then launch a published app from that client before you call setup done.'
})

const primaryPairingCta = computed(() => {
  if (!pairingEnabled.value) {
    return {
      label: 'Open Network Settings',
      href: '#/config#network',
    }
  }
  if (pairedClientCount.value > 0) {
    return {
      label: 'Review Saved Devices',
      href: '#/pin',
    }
  }
  return {
    label: 'Open Nova QR Pairing',
    href: '#/pin?method=OTP',
  }
})

const secondaryPairingCta = computed(() => {
  if (!pairingEnabled.value) {
    return {
      label: 'Open Pairing Page',
      href: '#/pin',
    }
  }
  if (pairedClientCount.value > 0) {
    return {
      label: 'Pair Another Client',
      href: '#/pin?method=OTP',
    }
  }
  if (trustedPairingEnabled.value && trustedSubnetCount.value > 0) {
    return {
      label: 'Open Trusted Network Pairing',
      href: '#/pin?method=TOFU',
    }
  }
  return {
    label: 'Open Manual PIN Pairing',
    href: '#/pin?method=PIN',
  }
})

function finishWizard() {
  document.location.href = './#/'
  document.location.reload()
}

onMounted(() => {
  detectWizardState()
})

function save() {
  if (passwordData.newPassword !== passwordData.confirmNewPassword) {
    error.value = "Password mismatch"
    return
  }
  error.value = null
  loading.value = true
  fetch("./api/password", {
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify(passwordData),
  }).then((r) => {
    loading.value = false
    if (r.status === 200) {
      r.json().then((rj) => {
        success.value = rj.status
        if (success.value !== true) {
          error.value = rj.error
        }
      })
    } else {
      error.value = "Internal Server Error"
    }
  })
}
</script>
