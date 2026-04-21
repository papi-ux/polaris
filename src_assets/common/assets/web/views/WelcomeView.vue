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
              <div class="mb-4 flex items-start justify-between gap-3 rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                <div class="font-medium">Store this account somewhere safe.</div>
                <InfoHint align="right" label="Credential safety">
                  Polaris will not surface the password again after setup. Keep the web UI credentials in a password manager before you continue.
                </InfoHint>
              </div>
              <form @submit.prevent="save" class="space-y-4">
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
              <div v-if="configLoading" class="rounded-2xl border border-storm/20 bg-void/45 px-4 py-8 text-center text-sm text-storm">Loading configuration…</div>
              <div v-else-if="configData" class="space-y-3">
                <div class="surface-subtle p-4">
                  <div class="text-sm text-storm">Video Encoder</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ configData.video_encoder || configData.encoder || 'Auto-detected' }}</div>
                </div>
                <div v-if="configData.adapter_name || configData.output_name" class="surface-subtle p-4">
                  <div class="text-sm text-storm">GPU / Display Adapter</div>
                  <div class="mt-2 text-base font-medium text-silver">{{ configData.adapter_name || configData.output_name || 'Default' }}</div>
                </div>
                <div class="flex items-center justify-between gap-3 rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  <span>Looks good if the encoder and adapter match your host.</span>
                  <InfoHint align="right" label="Runtime defaults">
                    You can fine-tune encoder and display behavior later in Configuration → Audio/Video. This step is only a quick sanity check.
                  </InfoHint>
                </div>
              </div>
              <div v-else class="rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                Could not load the configuration. You can still continue and revisit hardware settings later.
              </div>
            </div>

            <div v-if="currentStep === 2">
              <div class="space-y-3">
                <div class="surface-subtle p-4">
                  <div class="text-sm text-storm">Streaming Ports</div>
                  <div class="mt-2 text-base font-medium text-silver">
                    TCP: {{ configData?.port ?? '47989' }} (HTTPS: {{ configData?.port ? parseInt(configData.port) - 2 : '47984' }})
                  </div>
                  <div class="mt-1 text-base font-medium text-silver">
                    UDP: 47998-48000, 48002, 48010
                  </div>
                </div>
                <div class="flex items-center justify-between gap-3 rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                  <span><b>Firewall reminder:</b> allow the ports above before the first client test.</span>
                  <InfoHint align="right" label="Network path guidance">
                    Local-only streaming is usually enough at first. Add port forwarding only if you explicitly plan to stream outside your LAN.
                  </InfoHint>
                </div>
                <div class="rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  You can change the host port later in Configuration → Network.
                </div>
              </div>
            </div>

            <div v-if="currentStep === 3">
              <div class="space-y-3">
                <div class="rounded-2xl border border-storm/20 bg-void/45 p-5 text-center">
                  <div class="mb-3 text-4xl">&#x1F3AE;</div>
                  <div class="text-base font-medium text-silver">Start with the titles you actually want visible.</div>
                </div>
                <a href="#/apps" target="_blank" class="inline-flex h-10 items-center justify-center rounded-xl border border-storm/25 bg-deep/50 px-4 text-sm font-medium text-ice transition-[background-color,border-color,color] duration-200 hover:border-ice/30 hover:bg-twilight/35 no-underline">
                  Open Applications Page
                </a>
              </div>
            </div>

            <div v-if="currentStep === 4">
              <div class="space-y-3">
                <div class="surface-subtle p-4">
                  <div class="mb-2 text-sm font-medium text-silver">How to pair</div>
                  <ol class="list-inside list-decimal space-y-2 text-sm text-storm">
                    <li>Install <a href="https://moonlight-stream.org" target="_blank" class="text-ice hover:underline">Moonlight</a> or Nova on your client device.</li>
                    <li>Open the client and add this host’s IP address.</li>
                    <li>Use the PIN, QR, or trusted-pair flow Polaris exposes.</li>
                    <li>Finish pairing from the Clients &amp; Pairing page, then launch an app.</li>
                  </ol>
                </div>
                <a href="#/pin" target="_blank" class="inline-flex h-10 items-center justify-center rounded-xl border border-storm/25 bg-deep/50 px-4 text-sm font-medium text-ice transition-[background-color,border-color,color] duration-200 hover:border-ice/30 hover:bg-twilight/35 no-underline">
                  Open Clients &amp; Pairing
                </a>
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
import { ref, reactive } from 'vue'
import InfoHint from '../components/InfoHint.vue'

const currentStep = ref(0)
const error = ref(null)
const success = ref(false)
const loading = ref(false)
const configLoading = ref(false)
const configData = ref(null)

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

function nextStep() {
  if (currentStep.value === 0 && !success.value) return
  currentStep.value++
  if (currentStep.value === 1 && !configData.value) {
    loadConfig()
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

function finishWizard() {
  document.location.href = './#/'
  document.location.reload()
}

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
