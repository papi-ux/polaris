<template>
  <div class="min-h-screen bg-void flex items-center justify-center p-4">
    <div class="flex flex-col lg:flex-row gap-6 max-w-4xl w-full">
      <div class="bg-deep rounded-2xl border border-storm p-8 w-full max-w-md shadow-2xl">
        <header class="flex items-center gap-3 mb-6">
          <img src="/images/logo-polaris-45.png" class="h-10" alt="Polaris">
          <h1 class="text-2xl font-bold text-silver">{{ $t('welcome.greeting') }}</h1>
        </header>

        <!-- Stepper indicator -->
        <div class="flex items-center justify-center mb-8">
          <template v-for="(stepDef, idx) in steps" :key="idx">
            <div
              class="w-8 h-8 rounded-full flex items-center justify-center text-sm font-bold shrink-0 transition-colors"
              :class="stepClass(idx)"
            >
              <svg v-if="idx < currentStep" class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="3" d="M5 13l4 4L19 7"/>
              </svg>
              <span v-else>{{ idx + 1 }}</span>
            </div>
            <div
              v-if="idx < steps.length - 1"
              class="h-0.5 w-8 transition-colors"
              :class="idx < currentStep ? 'bg-storm' : 'bg-twilight'"
            ></div>
          </template>
        </div>

        <!-- Step 1: Create Credentials -->
        <div v-if="currentStep === 0">
          <h2 class="text-lg font-semibold text-silver mb-2">Create Credentials</h2>
          <p class="text-storm mb-3">{{ $t('welcome.create_creds') }}</p>
          <div class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-3 rounded-lg mb-4">
            {{ $t('welcome.create_creds_alert') }}
          </div>
          <form @submit.prevent="save" class="space-y-4">
            <div>
              <label for="usernameInput" class="block text-sm font-medium text-storm mb-1">{{ $t('_common.username') }}</label>
              <input type="text" class="w-full bg-void border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="usernameInput" autocomplete="username"
                v-model="passwordData.newUsername" />
            </div>
            <div>
              <label for="passwordInput" class="block text-sm font-medium text-storm mb-1">{{ $t('_common.password') }}</label>
              <input type="password" class="w-full bg-void border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="passwordInput" autocomplete="new-password"
                v-model="passwordData.newPassword" required />
            </div>
            <div>
              <label for="confirmPasswordInput" class="block text-sm font-medium text-storm mb-1">{{ $t('welcome.confirm_password') }}</label>
              <input type="password" class="w-full bg-void border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="confirmPasswordInput" autocomplete="new-password"
                v-model="passwordData.confirmNewPassword" required />
            </div>
            <button type="submit" class="w-full bg-ice text-void px-4 py-2 rounded-lg hover:bg-ice/80 transition font-semibold disabled:opacity-50" v-bind:disabled="loading">
              {{ $t('welcome.login') }}
            </button>
            <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg" v-if="error"><b>{{ $t('_common.error') }}</b> {{error}}</div>
            <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg" v-if="success">
              <b>{{ $t('_common.success') }}</b> Credentials saved! Click Next to continue setup.
            </div>
          </form>
        </div>

        <!-- Step 2: GPU Detection -->
        <div v-if="currentStep === 1">
          <h2 class="text-lg font-semibold text-silver mb-2">GPU & Encoder Detection</h2>
          <p class="text-storm mb-4">Polaris has detected the following encoding capabilities on your system.</p>
          <div v-if="configLoading" class="text-storm text-center py-8">Loading configuration...</div>
          <div v-else-if="configData" class="space-y-3">
            <div class="bg-twilight/50 border border-storm rounded-lg p-4">
              <div class="text-sm text-storm mb-1">Video Encoder</div>
              <div class="text-silver font-medium">{{ configData.video_encoder || configData.encoder || 'Auto-detected' }}</div>
            </div>
            <div v-if="configData.adapter_name || configData.output_name" class="bg-twilight/50 border border-storm rounded-lg p-4">
              <div class="text-sm text-storm mb-1">GPU / Display Adapter</div>
              <div class="text-silver font-medium">{{ configData.adapter_name || configData.output_name || 'Default' }}</div>
            </div>
            <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-3 rounded-lg text-sm">
              You can fine-tune encoder settings later in Configuration > Audio/Video.
            </div>
          </div>
          <div v-else class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-3 rounded-lg">
            Could not load configuration. You can configure encoding settings later.
          </div>
        </div>

        <!-- Step 3: Network Check -->
        <div v-if="currentStep === 2">
          <h2 class="text-lg font-semibold text-silver mb-2">Network Check</h2>
          <p class="text-storm mb-4">Ensure your network is ready for game streaming.</p>
          <div class="space-y-3">
            <div class="bg-twilight/50 border border-storm rounded-lg p-4">
              <div class="text-sm text-storm mb-1">Streaming Ports</div>
              <div class="text-silver font-medium">
                TCP: {{ configData?.port ?? '47989' }} (HTTPS: {{ configData?.port ? parseInt(configData.port) - 2 : '47984' }})
              </div>
              <div class="text-silver font-medium mt-1">
                UDP: 47998-48000, 48002, 48010
              </div>
            </div>
            <div class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-3 rounded-lg text-sm">
              <b>Firewall Reminder:</b> Make sure the ports above are allowed through your firewall. If you are behind a router, you may also need to set up port forwarding for remote streaming.
            </div>
            <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-3 rounded-lg text-sm">
              You can change the port in Configuration > Network.
            </div>
          </div>
        </div>

        <!-- Step 4: First App -->
        <div v-if="currentStep === 3">
          <h2 class="text-lg font-semibold text-silver mb-2">Add Your First App</h2>
          <p class="text-storm mb-4">Polaris streams your desktop and applications to Moonlight clients.</p>
          <div class="space-y-3">
            <div class="bg-twilight/50 border border-storm rounded-lg p-4 text-center">
              <div class="text-4xl mb-3">&#x1F3AE;</div>
              <div class="text-silver font-medium mb-2">Your apps will appear in the Applications page</div>
              <p class="text-storm text-sm">
                Add games and apps you want to stream. Polaris supports auto-detection of Steam games and custom application paths.
              </p>
            </div>
            <a href="#/apps" target="_blank" class="block text-center bg-storm/30 text-ice px-4 py-2 rounded-lg hover:bg-storm/50 transition no-underline text-sm">
              Open Applications Page &rarr;
            </a>
          </div>
        </div>

        <!-- Step 5: Pair Client -->
        <div v-if="currentStep === 4">
          <h2 class="text-lg font-semibold text-silver mb-2">Pair a Client</h2>
          <p class="text-storm mb-4">Connect your Moonlight client to start streaming.</p>
          <div class="space-y-3">
            <div class="bg-twilight/50 border border-storm rounded-lg p-4">
              <div class="text-silver font-medium mb-2">How to pair:</div>
              <ol class="text-storm text-sm space-y-2 list-decimal list-inside">
                <li>Install <a href="https://moonlight-stream.org" target="_blank" class="text-ice hover:underline">Moonlight</a> on your client device</li>
                <li>Open Moonlight and add this PC's IP address</li>
                <li>Moonlight will show a 4-digit PIN</li>
                <li>Enter the PIN on the Polaris Clients & Pairing page</li>
              </ol>
            </div>
            <a href="#/pin" target="_blank" class="block text-center bg-storm/30 text-ice px-4 py-2 rounded-lg hover:bg-storm/50 transition no-underline text-sm">
              Open Clients & Pairing Page &rarr;
            </a>
            <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-3 rounded-lg text-sm">
              You can also use OTP (one-time password) pairing from the Clients & Pairing page.
            </div>
          </div>
        </div>

        <!-- Navigation buttons -->
        <div class="flex justify-between mt-6">
          <button
            v-if="currentStep > 0"
            @click="currentStep--"
            class="bg-twilight text-silver px-4 py-2 rounded-lg hover:bg-storm/50 transition font-medium"
          >
            Back
          </button>
          <div v-else></div>
          <button
            v-if="currentStep < steps.length - 1"
            @click="nextStep"
            :disabled="currentStep === 0 && !success"
            class="bg-ice text-void px-4 py-2 rounded-lg hover:bg-ice/80 transition font-semibold disabled:opacity-50 disabled:cursor-not-allowed"
          >
            Next
          </button>
          <button
            v-else
            @click="finishWizard"
            class="bg-ice text-void px-4 py-2 rounded-lg hover:bg-ice/80 transition font-semibold"
          >
            Finish Setup
          </button>
        </div>
      </div>
      <div class="flex-1">
        <ResourceCard />
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

function stepClass(idx) {
  if (idx < currentStep.value) return 'bg-storm text-void'
  if (idx === currentStep.value) return 'bg-ice text-void'
  return 'bg-twilight text-storm'
}

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
