<template>
  <div class="relative min-h-screen overflow-hidden bg-background">
    <div class="pointer-events-none absolute inset-0 bg-[radial-gradient(circle_at_top_left,rgba(124,115,255,0.18),transparent_32%),radial-gradient(circle_at_bottom_right,rgba(200,214,229,0.08),transparent_24%)]"></div>
    <div class="relative mx-auto flex min-h-screen max-w-6xl items-center justify-center p-4 sm:p-6">
      <div class="grid w-full max-w-5xl gap-6 xl:grid-cols-[minmax(0,1.15fr)_340px]">
        <section class="glass rounded-[28px] border border-storm/30 p-6 shadow-2xl sm:p-8">
          <div class="flex items-center gap-3">
            <img src="/images/logo-polaris-45.png" class="h-11" alt="Polaris">
            <div>
              <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm/85">First-Time Setup</div>
              <h1 class="mt-1 text-3xl font-bold text-silver">{{ $t('welcome.greeting') }}</h1>
            </div>
          </div>

          <div class="mt-6 flex flex-col gap-5 border-b border-storm/20 pb-6">
            <p class="max-w-3xl text-sm leading-relaxed text-storm">
              This quick setup gets the host secured, checks the default runtime path, and points you to the two things that matter next: publishing apps and pairing a client.
            </p>

            <div class="flex flex-wrap gap-2">
              <div
                v-for="(stepDef, idx) in steps"
                :key="stepDef.title"
                class="rounded-full border px-3 py-1.5 text-sm transition-[background-color,border-color,color] duration-200"
                :class="idx === currentStep ? 'border-ice/40 bg-ice/10 text-ice' : idx < currentStep ? 'border-storm/30 bg-deep/45 text-silver' : 'border-storm/20 bg-deep/25 text-storm'"
              >
                <span class="font-medium">{{ idx + 1 }}.</span>
                <span class="ml-2">{{ stepDef.title }}</span>
              </div>
            </div>
          </div>

          <div class="mt-6 rounded-[24px] border border-storm/20 bg-deep/35 p-5 sm:p-6">
            <div class="mb-5">
              <div class="text-[10px] font-semibold uppercase tracking-[0.22em] text-storm">Step {{ currentStep + 1 }} of {{ steps.length }}</div>
              <h2 class="mt-2 text-2xl font-semibold text-silver">{{ steps[currentStep].title }}</h2>
            </div>

            <div v-if="currentStep === 0">
              <p class="mb-4 text-sm leading-relaxed text-storm">{{ $t('welcome.create_creds') }}</p>
              <div class="mb-4 rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                {{ $t('welcome.create_creds_alert') }}
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
              <p class="mb-4 text-sm leading-relaxed text-storm">Polaris detected the following host runtime defaults. You can fine-tune them later, but this gives you a quick sanity check before streaming.</p>
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
                <div class="rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  You can fine-tune encoder and display behavior later in Configuration → Audio/Video.
                </div>
              </div>
              <div v-else class="rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                Could not load the configuration. You can still continue and revisit hardware settings later.
              </div>
            </div>

            <div v-if="currentStep === 2">
              <p class="mb-4 text-sm leading-relaxed text-storm">Confirm the network path before pairing anything. Local-only streaming is usually enough at first; remote access can wait until the host is stable.</p>
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
                <div class="rounded-2xl border border-amber-300/25 bg-amber-300/10 px-4 py-3 text-sm text-amber-100">
                  <b>Firewall reminder:</b> allow the ports above before testing a new client. Add port forwarding only if you actually plan to stream beyond your LAN.
                </div>
                <div class="rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  You can change the host port later in Configuration → Network.
                </div>
              </div>
            </div>

            <div v-if="currentStep === 3">
              <p class="mb-4 text-sm leading-relaxed text-storm">Polaris is most useful when the library is intentional. Publish the apps you actually want to expose instead of dumping every launcher and helper tool into the client view.</p>
              <div class="space-y-3">
                <div class="rounded-2xl border border-storm/20 bg-void/45 p-5 text-center">
                  <div class="mb-3 text-4xl">&#x1F3AE;</div>
                  <div class="text-base font-medium text-silver">Applications become the client-facing launch surface</div>
                  <p class="mt-2 text-sm leading-relaxed text-storm">
                    Add games manually, scan supported launchers, and tune per-app behavior once entries exist.
                  </p>
                </div>
                <a href="#/apps" target="_blank" class="inline-flex h-10 items-center justify-center rounded-xl border border-storm/25 bg-deep/50 px-4 text-sm font-medium text-ice transition-[background-color,border-color,color] duration-200 hover:border-ice/30 hover:bg-twilight/35 no-underline">
                  Open Applications Page
                </a>
              </div>
            </div>

            <div v-if="currentStep === 4">
              <p class="mb-4 text-sm leading-relaxed text-storm">Once the host is secured and the library exists, pair a client and start streaming. Nova and Moonlight both work, but Nova exposes more Polaris-specific controls.</p>
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
                <div class="rounded-2xl border border-blue-400/20 bg-blue-400/10 px-4 py-3 text-sm text-blue-100">
                  Trusted pair and QR flows can be faster than manual PIN entry when the client supports them.
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
        </section>

        <aside class="space-y-4">
          <section class="section-card">
            <div class="section-kicker">Setup Path</div>
            <h2 class="section-title">What this wizard is covering</h2>
            <p class="section-copy">
              This flow is intentionally short. It secures the host first, then verifies enough of the runtime to let you move into real streaming work.
            </p>
            <div class="mt-5 space-y-2">
              <div
                v-for="(stepDef, idx) in steps"
                :key="stepDef.title"
                class="rounded-2xl border px-4 py-3 text-sm transition-[background-color,border-color,color] duration-200"
                :class="idx === currentStep ? 'border-ice/35 bg-ice/10 text-ice' : idx < currentStep ? 'border-storm/25 bg-deep/40 text-silver' : 'border-storm/15 bg-deep/25 text-storm'"
              >
                <span class="font-medium">{{ idx + 1 }}.</span>
                <span class="ml-2">{{ stepDef.title }}</span>
              </div>
            </div>
          </section>

          <section class="section-card">
            <div class="section-kicker">After Setup</div>
            <h2 class="section-title">Most hosts do these next</h2>
            <ul class="mt-4 space-y-2 text-sm leading-relaxed text-storm">
              <li>Check Mission Control once a real stream is running.</li>
              <li>Publish only the apps you want clients to see.</li>
              <li>Review pairing permissions before inviting other devices.</li>
            </ul>
          </section>

          <ResourceCard />
        </aside>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive } from 'vue'
import ResourceCard from '../ResourceCard.vue'

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
  { title: 'Credentials' },
  { title: 'GPU Detection' },
  { title: 'Network' },
  { title: 'First App' },
  { title: 'Pair Client' },
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
