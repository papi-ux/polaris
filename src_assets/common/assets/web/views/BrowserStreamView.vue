<template>
  <div class="space-y-5">
    <section class="settings-section">
      <div class="settings-section-header gap-4 lg:flex-row lg:items-start lg:justify-between">
        <div>
          <div class="section-kicker">Instant LAN Access</div>
          <h2 class="settings-section-title">Browser Stream</h2>
          <p class="settings-section-copy mt-2 max-w-3xl">
            Open a low-friction stream in this browser for touch, mouse, keyboard, launchers, desktop, and slower games. Use Nova or Moonlight for controller-first handheld play.
          </p>
        </div>
        <div class="flex flex-wrap gap-2">
          <span class="meta-pill">LAN only</span>
          <span class="meta-pill">Zero install</span>
          <span class="meta-pill border-amber-300/30 bg-amber-300/10 text-amber-200">Controller via Nova</span>
        </div>
      </div>

      <div class="grid gap-3 lg:grid-cols-2">
        <article class="surface-subtle p-4">
          <div class="text-xs font-semibold uppercase text-storm">Best for</div>
          <div class="mt-3 grid gap-2 text-sm text-silver sm:grid-cols-2">
            <span class="data-pill">Touch-first checks</span>
            <span class="data-pill">Desktop and launchers</span>
            <span class="data-pill">Strategy or card games</span>
            <span class="data-pill">Quick host smoke tests</span>
          </div>
        </article>
        <article class="surface-subtle border-amber-300/20 bg-amber-300/5 p-4">
          <div class="text-xs font-semibold uppercase text-amber-200">Use Nova or Moonlight for</div>
          <div class="mt-3 grid gap-2 text-sm text-silver sm:grid-cols-2">
            <span class="data-pill border-amber-300/20 bg-amber-300/10 text-amber-100">Retroid controls</span>
            <span class="data-pill border-amber-300/20 bg-amber-300/10 text-amber-100">Controller-first games</span>
            <span class="data-pill border-amber-300/20 bg-amber-300/10 text-amber-100">Competitive action</span>
            <span class="data-pill border-amber-300/20 bg-amber-300/10 text-amber-100">WAN streaming</span>
          </div>
        </article>
      </div>

      <div class="grid gap-3 md:grid-cols-4">
        <div class="surface-subtle p-4">
          <div class="text-xs font-semibold uppercase text-storm">Browser</div>
          <div class="mt-2 text-lg font-semibold text-silver">{{ browserSupported ? 'Ready' : 'Unsupported' }}</div>
          <div class="mt-2 text-xs text-storm">{{ browserSupported ? 'WebTransport and WebCodecs available.' : readiness.message }}</div>
        </div>
        <div class="surface-subtle p-4">
          <div class="text-xs font-semibold uppercase text-storm">Host</div>
          <div class="mt-2 text-lg font-semibold text-silver">{{ runtimeLabel }}</div>
          <div class="mt-2 text-xs text-storm">{{ readiness.message }}</div>
        </div>
        <div class="surface-subtle p-4">
          <div class="text-xs font-semibold uppercase text-storm">Input</div>
          <div class="mt-2 flex flex-wrap gap-1.5">
            <span
              v-for="input in inputSummary"
              :key="input.key"
              class="data-pill"
              :class="input.supported ? 'border-green-400/20 bg-green-400/10 text-green-200' : 'border-amber-300/20 bg-amber-300/10 text-amber-100'"
            >
              {{ input.label }} {{ input.supported ? 'yes' : (input.deferred ? 'deferred' : 'no') }}
            </span>
          </div>
        </div>
        <div class="surface-subtle p-4">
          <div class="text-xs font-semibold uppercase text-storm">Network</div>
          <div class="mt-2 text-lg font-semibold text-silver">{{ networkLabel }}</div>
          <div class="mt-2 text-xs text-storm">WebTransport uses a dedicated HTTPS/UDP port.</div>
        </div>
      </div>

      <div
        class="mt-4 rounded-lg border p-4 text-sm text-silver"
        :class="readiness.ready ? 'border-storm/20 bg-deep/40' : 'border-amber-300/30 bg-amber-300/10'"
      >
        {{ supportMessage }}
      </div>

      <div class="mt-4 rounded-lg border border-storm/20 bg-deep/40 p-4">
        <div class="flex flex-col gap-3 sm:flex-row sm:items-end sm:justify-between">
          <div class="min-w-0 flex-1">
            <label for="browser-stream-app" class="text-xs font-semibold uppercase text-storm">App</label>
            <select
              id="browser-stream-app"
              v-model="selectedAppUuid"
              class="mt-2 w-full rounded-lg border border-storm/25 bg-void px-3 py-2 text-sm text-silver focus:border-ice focus:outline-none disabled:cursor-not-allowed disabled:opacity-60"
              :disabled="Boolean(session) || connecting || appsLoading"
            >
              <option value="">{{ appsLoading ? 'Loading apps' : 'Choose an app' }}</option>
              <option v-for="app in apps" :key="app.uuid" :value="app.uuid">
                {{ formatBrowserStreamAppOption(app) }}
              </option>
            </select>
          </div>
          <router-link
            to="/pin?method=OTP"
            class="focus-ring inline-flex h-10 items-center justify-center rounded-lg border border-amber-300/25 bg-amber-300/10 px-3 text-sm font-medium text-amber-100 transition-colors hover:border-amber-200/50 hover:text-amber-50"
          >
            Nova/Moonlight setup
          </router-link>
        </div>
        <div v-if="selectedAppFit" class="mt-3 flex flex-col gap-2 rounded-lg border border-storm/20 bg-void/40 p-3 sm:flex-row sm:items-center sm:justify-between">
          <div class="text-sm text-storm">{{ selectedAppFit.description }}</div>
          <span class="meta-pill whitespace-nowrap" :class="toneClass(selectedAppFit.tone)">
            {{ selectedAppFit.label }}
          </span>
        </div>
        <div v-else class="mt-3 text-sm text-storm">
          Choose an app to see whether it is a practical browser fit.
        </div>
      </div>

      <div class="mt-4 rounded-lg border border-storm/20 bg-deep/40 p-4">
        <div class="flex flex-col gap-2 sm:flex-row sm:items-start sm:justify-between">
          <div>
            <div class="text-xs font-semibold uppercase text-storm">Stream profile</div>
            <div class="mt-1 text-sm text-silver">{{ selectedProfile.description }}</div>
          </div>
          <span class="meta-pill whitespace-nowrap">{{ selectedProfile.detail }}</span>
        </div>
        <div class="mt-3 grid gap-2 sm:grid-cols-3">
          <button
            v-for="profile in streamProfiles"
            :key="profile.id"
            type="button"
            class="focus-ring rounded-lg border px-3 py-3 text-left transition-colors disabled:cursor-not-allowed disabled:opacity-60"
            :class="profile.id === selectedProfile.id ? 'border-ice/60 bg-ice/10 text-ice' : 'border-storm/25 bg-void/40 text-silver hover:border-ice/40'"
            :disabled="Boolean(session) || connecting"
            :aria-pressed="profile.id === selectedProfile.id"
            @click="selectStreamProfile(profile.id)"
          >
            <div class="text-sm font-semibold">{{ profile.label }}</div>
            <div class="mt-1 text-xs text-storm">{{ profile.detail }} - {{ profile.backend.bitrateKbps / 1000 }} Mbps</div>
          </button>
        </div>
      </div>

      <div class="mt-4 grid gap-4 xl:grid-cols-[minmax(0,1fr)_20rem]">
        <div
          ref="playerShell"
          class="relative overflow-hidden bg-black"
          :class="gameModeActive ? 'h-[100dvh] w-[100dvw] rounded-none border-0' : 'min-h-[18rem] rounded-lg border border-storm/20 bg-void/70'"
        >
          <canvas
            ref="canvas"
            class="h-full w-full touch-none select-none bg-black object-contain outline-none"
            :class="gameModeActive ? 'min-h-0 rounded-none' : 'min-h-[18rem] rounded-lg'"
            tabindex="0"
            @contextmenu.prevent
            @pointerdown="handlePointerDown"
            @pointermove="handlePointerMove"
            @pointerup="handlePointerUp"
            @pointercancel="handlePointerCancel"
            @wheel.prevent="handleWheel"
            @keydown="handleKeyDown"
            @keyup="handleKeyUp"
          ></canvas>
          <div
            v-if="gameModeActive"
            class="pointer-events-none absolute inset-0 z-10 flex flex-col justify-between p-3 opacity-0 transition-opacity duration-200 sm:p-4"
            :class="gameModeOverlayVisible ? 'opacity-100' : 'opacity-0'"
          >
            <div class="flex items-start justify-between gap-3">
              <div class="min-w-0 rounded-lg border border-black/40 bg-black/70 px-3 py-2 text-xs text-silver shadow-lg backdrop-blur">
                <div class="font-semibold uppercase text-storm">Game Mode</div>
                <div class="mt-1 truncate text-silver">{{ session?.session?.app_name || selectedApp?.name || 'Browser Stream' }}</div>
              </div>
              <div
                class="flex shrink-0 gap-2"
                :class="gameModeOverlayVisible ? 'pointer-events-auto' : 'pointer-events-none'"
              >
                <button
                  type="button"
                  class="focus-ring rounded-lg border border-white/20 bg-black/70 px-4 py-3 text-sm font-semibold text-silver shadow-lg backdrop-blur transition-colors hover:border-ice/60 hover:text-ice sm:px-3 sm:py-2 sm:text-xs"
                  @click="toggleGameModeStats"
                >
                  {{ showGameModeStats ? 'Hide stats' : 'Stats' }}
                </button>
                <button
                  type="button"
                  class="focus-ring rounded-lg border border-white/20 bg-black/70 px-4 py-3 text-sm font-semibold text-silver shadow-lg backdrop-blur transition-colors hover:border-amber-300/60 hover:text-amber-100 sm:px-3 sm:py-2 sm:text-xs"
                  @click="stop"
                >
                  Stop
                </button>
                <button
                  type="button"
                  class="focus-ring rounded-lg border border-white/20 bg-black/70 px-4 py-3 text-sm font-semibold text-silver shadow-lg backdrop-blur transition-colors hover:border-ice/60 hover:text-ice sm:px-3 sm:py-2 sm:text-xs"
                  @click="exitGameMode"
                >
                  Exit
                </button>
              </div>
            </div>
            <div class="flex items-end justify-between gap-3">
              <div class="min-w-0 rounded-lg border border-black/40 bg-black/70 px-3 py-2 text-xs text-silver shadow-lg backdrop-blur">
                <div class="font-semibold uppercase text-storm">{{ selectedProfile.label }}</div>
                <div class="mt-1 truncate text-silver">{{ streamHealthLabel }}</div>
              </div>
              <dl
                v-if="showGameModeStats"
                class="grid min-w-48 grid-cols-2 gap-x-3 gap-y-1 rounded-lg border border-black/40 bg-black/75 p-3 text-xs text-silver shadow-lg backdrop-blur"
              >
                <dt class="text-storm">FPS</dt><dd class="text-right">{{ stats.fps }}</dd>
                <dt class="text-storm">Bitrate</dt><dd class="text-right">{{ stats.bitrateKbps }} kbps</dd>
                <dt class="text-storm">Dropped</dt><dd class="text-right">{{ stats.droppedFrames }}</dd>
                <dt class="text-storm">Queue</dt><dd class="text-right">{{ stats.decodeQueue }}</dd>
                <dt class="text-storm">Audio</dt><dd class="text-right">{{ stats.audioQueue }}</dd>
              </dl>
            </div>
          </div>
        </div>
        <div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-1">
          <div class="surface-subtle p-4">
            <div class="text-xs font-semibold uppercase text-storm">Video</div>
            <div class="mt-2 text-sm text-silver">{{ status.codecs?.video?.label || 'H.264 SDR 8-bit' }}</div>
          </div>
          <div class="surface-subtle p-4">
            <div class="text-xs font-semibold uppercase text-storm">Audio</div>
            <div class="mt-2 text-sm text-silver">{{ status.codecs?.audio?.label || 'Opus' }}</div>
          </div>
          <div class="surface-subtle p-4">
            <div class="text-xs font-semibold uppercase text-storm">Live stats</div>
            <dl class="mt-2 grid grid-cols-2 gap-x-3 gap-y-1 text-sm text-silver">
              <dt class="text-storm">FPS</dt><dd class="text-right">{{ stats.fps }}</dd>
              <dt class="text-storm">Bitrate</dt><dd class="text-right">{{ stats.bitrateKbps }} kbps</dd>
              <dt class="text-storm">Dropped</dt><dd class="text-right">{{ stats.droppedFrames }}</dd>
              <dt class="text-storm">Queue</dt><dd class="text-right">{{ stats.decodeQueue }}</dd>
              <dt class="text-storm">Audio Queue</dt><dd class="text-right">{{ stats.audioQueue }}</dd>
              <dt class="text-storm">Health</dt><dd class="text-right">{{ streamHealthLabel }}</dd>
            </dl>
          </div>
        </div>
      </div>

      <div v-if="sessionMessage" class="mt-4 rounded-lg border border-storm/20 bg-deep/40 p-4 text-sm text-silver">
        {{ sessionMessage }}
      </div>

      <div class="mt-4 flex flex-wrap gap-2">
        <button
          type="button"
          class="focus-ring rounded-lg bg-ice px-4 py-2 text-sm font-semibold text-void transition-colors hover:bg-ice/90 disabled:cursor-not-allowed disabled:opacity-50"
          :disabled="!canStart || connecting"
          @click="start"
        >
          {{ connecting ? 'Starting' : 'Start browser stream' }}
        </button>
        <button
          type="button"
          class="focus-ring rounded-lg border border-storm/25 bg-deep px-4 py-2 text-sm font-medium text-silver transition-colors hover:border-ice/40 hover:text-ice disabled:cursor-not-allowed disabled:opacity-50"
          :disabled="!session"
          @click="stop"
        >
          Stop
        </button>
        <button
          type="button"
          class="focus-ring rounded-lg border border-storm/25 bg-deep px-4 py-2 text-sm font-medium text-silver transition-colors hover:border-ice/40 hover:text-ice disabled:cursor-not-allowed disabled:opacity-50"
          :disabled="!session || !fullscreenSupported"
          @click="enterGameMode"
        >
          Game Mode
        </button>
        <button
          type="button"
          class="focus-ring rounded-lg border border-storm/25 bg-deep px-4 py-2 text-sm font-medium text-silver transition-colors hover:border-ice/40 hover:text-ice"
          @click="refreshAll"
        >
          Refresh
        </button>
        <router-link
          to="/config"
          class="focus-ring rounded-lg border border-storm/25 bg-deep px-4 py-2 text-sm font-medium text-silver transition-colors hover:border-ice/40 hover:text-ice"
        >
          Settings
        </router-link>
      </div>
    </section>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import {
  browserStreamInputSummary,
  browserStreamIsSupported,
  browserStreamProfileById,
  browserStreamProfiles,
  browserStreamReadiness,
  browserStreamAppFit,
  detectBrowserStreamSupport,
  formatBrowserStreamAppOption,
} from '../browser-stream-support.js'
import {
  BrowserStreamCodec,
  BrowserStreamFlags,
  BrowserStreamKind,
  certHashHexToBytes,
  createBrowserStreamFrameAssembler,
  decodeBrowserStreamDatagram,
  encodeBrowserStreamControlFrame,
} from '../browser-stream-protocol.js'

const status = ref({
  build_enabled: false,
  config_enabled: false,
  available: false,
  state: 'loading',
  message: '',
  codecs: null,
  input: null,
  transport: null,
})
const browserSupport = detectBrowserStreamSupport()
const browserSupported = browserStreamIsSupported(browserSupport)
const connecting = ref(false)
const session = ref(null)
const sessionMessage = ref('')
const apps = ref([])
const appsLoading = ref(false)
const selectedAppUuid = ref(window.sessionStorage.getItem('browser-stream-app-uuid') || '')
const selectedProfileId = ref(window.sessionStorage.getItem('browser-stream-profile-id') || 'balanced')
const canvas = ref(null)
const playerShell = ref(null)
const gameModeActive = ref(false)
const gameModeControlsVisible = ref(false)
const showGameModeStats = ref(false)
let transport = null
let datagramReader = null
let controlStreamWriter = null
let videoDecoder = null
let audioDecoder = null
let audioContext = null
let audioGain = null
let audioNextTime = 0
let hasVideoKeyframe = false
let inputFlushTimer = null
let inputFlushInFlight = false
let pendingInputEvents = []
let gameModeControlsTimer = null
let frameAssembler = createBrowserStreamFrameAssembler(browserStreamProfileById(selectedProfileId.value).frameAssembler)
let bitrateWindowStart = performance.now()
let bitrateWindowBytes = 0
let fpsWindowStart = performance.now()
let fpsWindowFrames = 0
const stats = reactive({
  fps: 0,
  bitrateKbps: 0,
  droppedFrames: 0,
  decodeQueue: 0,
  audioQueue: 0,
})

const runtimeLabel = computed(() => {
  if (status.value.available) return 'Ready'
  if (status.value.state === 'disabled') return 'Disabled'
  if (status.value.state === 'not_built') return 'Not built'
  if (status.value.state === 'backend_pending') return 'Standby'
  return 'Pending'
})

const readiness = computed(() => browserStreamReadiness(browserSupport, status.value))

const inputSummary = computed(() => browserStreamInputSummary(status.value.input || {
  touch: true,
  pointer: true,
  keyboard: true,
  wheel: true,
  gamepad: false,
  gamepad_reserved: true,
}))

const networkLabel = computed(() => {
  const port = status.value.transport?.port || 47992
  return status.value.transport?.lan_only === false ? `Port ${port}` : `LAN:${port}`
})

const selectedApp = computed(() => apps.value.find((app) => app.uuid === selectedAppUuid.value) || null)
const selectedAppFit = computed(() => (selectedApp.value ? browserStreamAppFit(selectedApp.value) : null))
const streamProfiles = browserStreamProfiles
const selectedProfile = computed(() => browserStreamProfileById(selectedProfileId.value))
const gameModeOverlayVisible = computed(() => gameModeControlsVisible.value || showGameModeStats.value)
const fullscreenSupported = computed(() => (
  typeof document !== 'undefined' &&
  Boolean(document.fullscreenEnabled) &&
  typeof playerShell.value?.requestFullscreen === 'function'
))

const streamHealthLabel = computed(() => {
  if (!session.value) return selectedProfile.value.label
  if (stats.decodeQueue > selectedProfile.value.maxVideoDecodeQueue) return 'Decoder queue high'
  if (stats.audioQueue > selectedProfile.value.maxAudioDecodeQueue) return 'Audio queue high'
  if (stats.droppedFrames > 0) return 'Dropping late frames'
  if (stats.fps > 0) return 'Stable'
  return 'Starting'
})

const supportMessage = computed(() => {
  if (!readiness.value.ready) return readiness.value.message
  if (session.value) {
    return 'Browser Stream transport connected.'
  }
  if (status.value.build_enabled && status.value.config_enabled && !selectedAppUuid.value) {
    return appsLoading.value ? 'Loading applications.' : 'Choose an app to start an instant LAN browser session.'
  }
  if (selectedAppFit.value) {
    return `${selectedApp.value?.name || 'Selected app'}: ${selectedAppFit.value.description}`
  }
  return readiness.value.message
})

const canStart = computed(() => (
  !session.value &&
  browserSupported &&
  status.value.build_enabled &&
  status.value.config_enabled &&
  Boolean(selectedAppUuid.value)
))

function toneClass(tone) {
  if (tone === 'success') return 'border-green-400/25 bg-green-400/10 text-green-200'
  if (tone === 'warning') return 'border-amber-300/30 bg-amber-300/10 text-amber-100'
  if (tone === 'danger') return 'border-red-400/30 bg-red-400/10 text-red-100'
  return ''
}

function configureFrameAssemblerForProfile() {
  frameAssembler = createBrowserStreamFrameAssembler(selectedProfile.value.frameAssembler)
}

function selectStreamProfile(profileId) {
  if (session.value || connecting.value) return
  const profile = browserStreamProfileById(profileId)
  selectedProfileId.value = profile.id
  window.sessionStorage.setItem('browser-stream-profile-id', profile.id)
  configureFrameAssemblerForProfile()
}

function clearGameModeControlsTimer() {
  if (gameModeControlsTimer) {
    window.clearTimeout(gameModeControlsTimer)
    gameModeControlsTimer = null
  }
}

function scheduleGameModeControlsHide() {
  clearGameModeControlsTimer()
  if (!gameModeActive.value || showGameModeStats.value) return
  gameModeControlsTimer = window.setTimeout(() => {
    gameModeControlsVisible.value = false
    gameModeControlsTimer = null
  }, 2600)
}

function revealGameModeControls({ hold = false } = {}) {
  if (!gameModeActive.value) return
  gameModeControlsVisible.value = true
  if (hold) {
    clearGameModeControlsTimer()
  } else {
    scheduleGameModeControlsHide()
  }
}

function updateGameModeState() {
  const isActive = typeof document !== 'undefined' && document.fullscreenElement === playerShell.value
  gameModeActive.value = isActive
  if (isActive) {
    revealGameModeControls()
  } else {
    clearGameModeControlsTimer()
    gameModeControlsVisible.value = false
    showGameModeStats.value = false
  }
}

async function enterGameMode() {
  if (!session.value) return
  if (!fullscreenSupported.value || !playerShell.value) {
    sessionMessage.value = 'This browser cannot enter Browser Stream Game Mode.'
    return
  }

  try {
    await playerShell.value.requestFullscreen({ navigationUI: 'hide' })
    gameModeActive.value = true
    revealGameModeControls()
    canvas.value?.focus({ preventScroll: true })
  } catch {
    sessionMessage.value = 'Browser Stream Game Mode was blocked by the browser.'
  }
}

async function exitGameMode() {
  if (typeof document === 'undefined') return
  try {
    if (document.fullscreenElement === playerShell.value && typeof document.exitFullscreen === 'function') {
      await document.exitFullscreen()
    }
  } catch {
    // Ignore fullscreen exit races; the browser will emit fullscreenchange if state changed.
  }
  clearGameModeControlsTimer()
  gameModeActive.value = false
  gameModeControlsVisible.value = false
  showGameModeStats.value = false
  canvas.value?.focus({ preventScroll: true })
}

function toggleGameModeStats() {
  showGameModeStats.value = !showGameModeStats.value
  revealGameModeControls({ hold: showGameModeStats.value })
}

function resetStats() {
  configureFrameAssemblerForProfile()
  stats.fps = 0
  stats.bitrateKbps = 0
  stats.droppedFrames = 0
  stats.decodeQueue = 0
  stats.audioQueue = 0
  bitrateWindowStart = performance.now()
  bitrateWindowBytes = 0
  fpsWindowStart = performance.now()
  fpsWindowFrames = 0
}

function mouseButtonFromDom(button) {
  if (button === 1) return 2
  if (button === 2) return 3
  if (button === 3) return 4
  if (button === 4) return 5
  return 1
}

const keyCodeMap = {
  Backspace: 0x08,
  Tab: 0x09,
  Enter: 0x0d,
  ShiftLeft: 0xa0,
  ShiftRight: 0xa1,
  ControlLeft: 0xa2,
  ControlRight: 0xa3,
  AltLeft: 0xa4,
  AltRight: 0xa5,
  Pause: 0x13,
  CapsLock: 0x14,
  Escape: 0x1b,
  Space: 0x20,
  PageUp: 0x21,
  PageDown: 0x22,
  End: 0x23,
  Home: 0x24,
  ArrowLeft: 0x25,
  ArrowUp: 0x26,
  ArrowRight: 0x27,
  ArrowDown: 0x28,
  Insert: 0x2d,
  Delete: 0x2e,
  MetaLeft: 0x5b,
  MetaRight: 0x5c,
  ContextMenu: 0x5d,
  NumpadMultiply: 0x6a,
  NumpadAdd: 0x6b,
  NumpadSubtract: 0x6d,
  NumpadDecimal: 0x6e,
  NumpadDivide: 0x6f,
  NumLock: 0x90,
  ScrollLock: 0x91,
  Semicolon: 0xba,
  Equal: 0xbb,
  Comma: 0xbc,
  Minus: 0xbd,
  Period: 0xbe,
  Slash: 0xbf,
  Backquote: 0xc0,
  BracketLeft: 0xdb,
  Backslash: 0xdc,
  BracketRight: 0xdd,
  Quote: 0xde,
}

function keyCodeFromEvent(event) {
  if (/^Key[A-Z]$/.test(event.code)) return event.code.charCodeAt(3)
  if (/^Digit[0-9]$/.test(event.code)) return event.code.charCodeAt(5)
  if (/^Numpad[0-9]$/.test(event.code)) return 0x60 + Number(event.code.slice(6))
  if (/^F([1-9]|1[0-2])$/.test(event.code)) return 0x6f + Number(event.code.slice(1))
  return keyCodeMap[event.code] || 0
}

function keyModifiers(event) {
  let modifiers = 0
  if (event.shiftKey) modifiers |= 0x01
  if (event.ctrlKey) modifiers |= 0x02
  if (event.altKey) modifiers |= 0x04
  if (event.metaKey) modifiers |= 0x08
  return modifiers
}

function pointerEventPayload(event, action) {
  const target = canvas.value
  if (!target) return null
  const rect = target.getBoundingClientRect()
  if (rect.width <= 0 || rect.height <= 0) return null
  return {
    type: 'pointer',
    action,
    pointer_id: event.pointerId || 0,
    pointer_type: event.pointerType || 'mouse',
    x: Math.min(Math.max(event.clientX - rect.left, 0), rect.width),
    y: Math.min(Math.max(event.clientY - rect.top, 0), rect.height),
    width: rect.width,
    height: rect.height,
    button: mouseButtonFromDom(event.button),
    buttons: event.buttons || 0,
    pressure: typeof event.pressure === 'number' ? event.pressure : (action === 'up' ? 0 : 1),
  }
}

function queueInputEvent(inputEvent) {
  if (!session.value || !inputEvent) return

  if (inputEvent.type === 'pointer' && inputEvent.action === 'move') {
    for (let index = pendingInputEvents.length - 1; index >= 0; index -= 1) {
      const queued = pendingInputEvents[index]
      if (queued.type === 'pointer' && queued.action === 'move' && queued.pointer_id === inputEvent.pointer_id) {
        pendingInputEvents.splice(index, 1)
        break
      }
    }
  }

  pendingInputEvents.push(inputEvent)
  if (pendingInputEvents.length >= 16) {
    flushInputEvents()
    return
  }
  if (!inputFlushTimer) {
    inputFlushTimer = window.setTimeout(flushInputEvents, 8)
  }
}

async function flushInputEvents() {
  if (inputFlushTimer) {
    window.clearTimeout(inputFlushTimer)
    inputFlushTimer = null
  }
  if (inputFlushInFlight || !pendingInputEvents.length || !session.value || !controlStreamWriter) return

  const events = pendingInputEvents.splice(0, pendingInputEvents.length)
  inputFlushInFlight = true
  try {
    await controlStreamWriter.write(encodeBrowserStreamControlFrame({
      type: 'input_events',
      events,
    }))
  } catch {
    sessionMessage.value = 'Browser Stream input control stream closed.'
  } finally {
    inputFlushInFlight = false
    if (pendingInputEvents.length && session.value) {
      inputFlushTimer = window.setTimeout(flushInputEvents, 8)
    }
  }
}

function clearInputQueue() {
  if (inputFlushTimer) {
    window.clearTimeout(inputFlushTimer)
    inputFlushTimer = null
  }
  pendingInputEvents = []
  inputFlushInFlight = false
}

function handlePointerDown(event) {
  if (!session.value) return
  event.preventDefault()
  revealGameModeControls()
  canvas.value?.focus({ preventScroll: true })
  try {
    event.currentTarget?.setPointerCapture?.(event.pointerId)
  } catch {
    // Capture can fail if the browser has already ended this pointer sequence.
  }
  queueInputEvent(pointerEventPayload(event, 'down'))
}

function handlePointerMove(event) {
  if (!session.value) return
  event.preventDefault()
  revealGameModeControls()
  queueInputEvent(pointerEventPayload(event, 'move'))
}

function handlePointerUp(event) {
  if (!session.value) return
  event.preventDefault()
  revealGameModeControls()
  try {
    event.currentTarget?.releasePointerCapture?.(event.pointerId)
  } catch {
    // Ignore release races; the matching button-up event still gets sent.
  }
  queueInputEvent(pointerEventPayload(event, 'up'))
}

function handlePointerCancel(event) {
  if (!session.value) return
  event.preventDefault()
  revealGameModeControls()
  try {
    event.currentTarget?.releasePointerCapture?.(event.pointerId)
  } catch {
    // Ignore release races; the matching cancel event still gets sent.
  }
  queueInputEvent(pointerEventPayload(event, 'cancel'))
}

function handleWheel(event) {
  if (!session.value) return
  revealGameModeControls()
  queueInputEvent({
    type: 'wheel',
    delta_x: event.deltaX || 0,
    delta_y: event.deltaY || 0,
  })
}

function handleKeyDown(event) {
  if (!session.value) return
  event.preventDefault()
  revealGameModeControls()
  if (event.repeat) return
  const keyCode = keyCodeFromEvent(event)
  if (!keyCode) return
  queueInputEvent({
    type: 'key',
    action: 'down',
    key_code: keyCode,
    modifiers: keyModifiers(event),
  })
}

function handleKeyUp(event) {
  if (!session.value) return
  event.preventDefault()
  revealGameModeControls()
  const keyCode = keyCodeFromEvent(event)
  if (!keyCode) return
  queueInputEvent({
    type: 'key',
    action: 'up',
    key_code: keyCode,
    modifiers: keyModifiers(event),
  })
}

function markStatusReadyFromSession(streamSession) {
  status.value = {
    ...status.value,
    build_enabled: true,
    config_enabled: true,
    available: true,
    state: 'ready',
    message: 'Browser Stream transport connected.',
    codecs: streamSession?.codecs || status.value.codecs,
  }
}

function recordBitrate(byteLength) {
  bitrateWindowBytes += byteLength
  const now = performance.now()
  const elapsed = now - bitrateWindowStart
  if (elapsed >= 1000) {
    stats.bitrateKbps = Math.round((bitrateWindowBytes * 8) / elapsed)
    bitrateWindowBytes = 0
    bitrateWindowStart = now
  }
}

function recordDecodedFrame() {
  fpsWindowFrames += 1
  const now = performance.now()
  const elapsed = now - fpsWindowStart
  if (elapsed >= 1000) {
    stats.fps = Math.round((fpsWindowFrames * 1000) / elapsed)
    fpsWindowFrames = 0
    fpsWindowStart = now
  }
}

function drawVideoFrame(frame) {
  try {
    const target = canvas.value
    if (!target) return
    const width = frame.displayWidth || frame.codedWidth || 1280
    const height = frame.displayHeight || frame.codedHeight || 720
    if (target.width !== width) target.width = width
    if (target.height !== height) target.height = height
    const context = target.getContext('2d')
    context.drawImage(frame, 0, 0, width, height)
    recordDecodedFrame()
  } finally {
    frame.close()
    stats.decodeQueue = videoDecoder?.decodeQueueSize || 0
  }
}

function resetVideoPipeline(message = '') {
  if (videoDecoder && videoDecoder.state !== 'closed') {
    videoDecoder.close()
  }
  videoDecoder = null
  hasVideoKeyframe = false
  frameAssembler.reset()
  stats.decodeQueue = 0
  if (message) {
    sessionMessage.value = message
  }
}

async function ensureAudioContext() {
  if (!audioContext) {
    const AudioContextCtor = window.AudioContext || window.webkitAudioContext
    if (!AudioContextCtor) {
      throw new Error('This browser cannot play Browser Stream audio.')
    }
    audioContext = new AudioContextCtor({
      latencyHint: selectedProfile.value.audioLatencyHint,
      sampleRate: 48000,
    })
    audioGain = audioContext.createGain()
    audioGain.gain.value = 1
    audioGain.connect(audioContext.destination)
  }
  if (audioContext.state === 'suspended') {
    await audioContext.resume()
  }
}

function resetAudioPipeline(message = '') {
  if (audioDecoder && audioDecoder.state !== 'closed') {
    audioDecoder.close()
  }
  audioDecoder = null
  audioNextTime = 0
  stats.audioQueue = 0
  if (message) {
    sessionMessage.value = message
  }
}

async function configureVideoDecoder(streamSession) {
  resetVideoPipeline()
  const baseConfig = {
    codec: streamSession?.codecs?.video?.webcodecs || status.value.codecs?.video?.webcodecs || 'avc1.640020',
    optimizeForLatency: true,
  }
  if (streamSession?.video?.width && streamSession?.video?.height) {
    baseConfig.codedWidth = streamSession.video.width
    baseConfig.codedHeight = streamSession.video.height
  }

  let config = { ...baseConfig, hardwareAcceleration: 'prefer-hardware' }
  if (typeof VideoDecoder.isConfigSupported === 'function') {
    const candidates = [
      config,
      { ...baseConfig, hardwareAcceleration: 'no-preference' },
      { ...baseConfig, hardwareAcceleration: 'prefer-software' },
      baseConfig,
    ]
    config = null
    for (const candidate of candidates) {
      const support = await VideoDecoder.isConfigSupported(candidate)
      if (support.supported) {
        config = support.config || candidate
        break
      }
    }
    if (!config) {
      throw new Error('This browser cannot decode Browser Stream H.264 video.')
    }
  }

  videoDecoder = new VideoDecoder({
    output: drawVideoFrame,
    error(error) {
      stats.droppedFrames += frameAssembler.pendingCount
      resetVideoPipeline(`Video decoder reset: ${error.message}`)
    },
  })
  videoDecoder.addEventListener?.('dequeue', () => {
    stats.decodeQueue = videoDecoder?.decodeQueueSize || 0
  })
  videoDecoder.configure(config)
}

async function configureAudioDecoder(streamSession) {
  resetAudioPipeline()
  await ensureAudioContext()

  const baseConfig = {
    codec: streamSession?.codecs?.audio?.webcodecs || status.value.codecs?.audio?.webcodecs || 'opus',
    sampleRate: streamSession?.audio?.sample_rate || 48000,
    numberOfChannels: streamSession?.audio?.channels || 2,
  }

  let config = baseConfig
  if (typeof AudioDecoder.isConfigSupported === 'function') {
    const support = await AudioDecoder.isConfigSupported(baseConfig)
    if (!support.supported) {
      throw new Error('This browser cannot decode Browser Stream Opus audio.')
    }
    config = support.config || baseConfig
  }

  audioDecoder = new AudioDecoder({
    output: playDecodedAudio,
    error(error) {
      resetAudioPipeline(`Audio decoder reset: ${error.message}`)
    },
  })
  audioDecoder.addEventListener?.('dequeue', () => {
    stats.audioQueue = audioDecoder?.decodeQueueSize || 0
  })
  audioDecoder.configure(config)
}

function decodeVideoFrame(frame) {
  if (frame.kind !== BrowserStreamKind.video || frame.codec !== BrowserStreamCodec.h264) {
    return
  }

  const isKeyframe = Boolean(frame.flags & BrowserStreamFlags.keyframe)
  if (!hasVideoKeyframe && !isKeyframe) {
    stats.droppedFrames += 1
    return
  }
  if (!videoDecoder || videoDecoder.state === 'closed') {
    stats.droppedFrames += 1
    return
  }
  if (videoDecoder.decodeQueueSize > selectedProfile.value.maxVideoDecodeQueue && !isKeyframe) {
    stats.droppedFrames += 1
    return
  }

  hasVideoKeyframe = hasVideoKeyframe || isKeyframe
  recordBitrate(frame.payload.byteLength)
  videoDecoder.decode(new EncodedVideoChunk({
    type: isKeyframe ? 'key' : 'delta',
    timestamp: frame.timestampUs,
    data: frame.payload,
  }))
  stats.decodeQueue = videoDecoder.decodeQueueSize
}

function audioDataToBuffer(audioData) {
  const channels = audioData.numberOfChannels || 2
  const frames = audioData.numberOfFrames
  const sampleRate = audioData.sampleRate || 48000
  const buffer = audioContext.createBuffer(channels, frames, sampleRate)
  const format = audioData.format || ''

  if (format.includes('planar')) {
    for (let channel = 0; channel < channels; channel += 1) {
      audioData.copyTo(buffer.getChannelData(channel), { planeIndex: channel, format: 'f32-planar' })
    }
    return buffer
  }

  try {
    const interleaved = new Float32Array(frames * channels)
    audioData.copyTo(interleaved, { planeIndex: 0, format: 'f32' })
    for (let frame = 0; frame < frames; frame += 1) {
      for (let channel = 0; channel < channels; channel += 1) {
        buffer.getChannelData(channel)[frame] = interleaved[(frame * channels) + channel]
      }
    }
    return buffer
  } catch {
    for (let channel = 0; channel < channels; channel += 1) {
      audioData.copyTo(buffer.getChannelData(channel), { planeIndex: channel, format: 'f32-planar' })
    }
    return buffer
  }
}

function playDecodedAudio(audioData) {
  try {
    if (!audioContext || !audioGain) return
    const buffer = audioDataToBuffer(audioData)
    const source = audioContext.createBufferSource()
    source.buffer = buffer
    source.connect(audioGain)

    const now = audioContext.currentTime
    if (
      audioNextTime < now + selectedProfile.value.audioMinLeadSeconds ||
      audioNextTime > now + selectedProfile.value.audioMaxLeadSeconds
    ) {
      audioNextTime = now + selectedProfile.value.audioStartLeadSeconds
    }
    source.start(audioNextTime)
    audioNextTime += buffer.duration
  } finally {
    audioData.close()
    stats.audioQueue = audioDecoder?.decodeQueueSize || 0
  }
}

function decodeAudioFrame(frame) {
  if (frame.kind !== BrowserStreamKind.audio || frame.codec !== BrowserStreamCodec.opus) {
    return
  }
  if (!audioDecoder || audioDecoder.state === 'closed') {
    stats.droppedFrames += 1
    return
  }
  if (audioDecoder.decodeQueueSize > selectedProfile.value.maxAudioDecodeQueue) {
    stats.droppedFrames += 1
    return
  }

  recordBitrate(frame.payload.byteLength)
  audioDecoder.decode(new EncodedAudioChunk({
    type: 'key',
    timestamp: frame.timestampUs,
    data: frame.payload,
  }))
  stats.audioQueue = audioDecoder.decodeQueueSize
}

function handleMediaDatagram(datagram) {
  const result = frameAssembler.push(datagram)
  stats.droppedFrames += result.droppedFrames
  if (result.frame) {
    if (result.frame.kind === BrowserStreamKind.audio) {
      decodeAudioFrame(result.frame)
    } else {
      decodeVideoFrame(result.frame)
    }
  }
}

async function stopBackendSession(token) {
  if (!token) return
  await fetch('./api/browser-stream/session/stop', {
    method: 'POST',
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ session_token: token }),
  }).catch(() => {})
}

async function openControlStream() {
  if (!transport || typeof transport.createBidirectionalStream !== 'function') {
    throw new Error('This browser cannot open Browser Stream control streams.')
  }

  const stream = await transport.createBidirectionalStream()
  controlStreamWriter = stream.writable.getWriter()
}

async function closeTransport() {
  const activeTransport = transport
  const activeControlWriter = controlStreamWriter
  controlStreamWriter = null
  clearInputQueue()
  if (activeControlWriter) {
    await activeControlWriter.close().catch(() => {})
    activeControlWriter.releaseLock?.()
  }
  if (datagramReader) {
    datagramReader.cancel().catch(() => {})
    datagramReader = null
  }
  if (activeTransport) {
    activeTransport.close()
    transport = null
    await Promise.race([
      activeTransport.closed.catch(() => {}),
      new Promise((resolve) => {
        window.setTimeout(resolve, 500)
      }),
    ])
  }
  resetAudioPipeline()
}

async function refresh() {
  try {
    const response = await fetch('./api/browser-stream/status', { credentials: 'include' })
    if (!response.ok) {
      throw new Error('status request failed')
    }
    status.value = await response.json()
  } catch {
    if (session.value) {
      markStatusReadyFromSession(session.value)
      return
    }
    status.value = {
      build_enabled: false,
      config_enabled: false,
      available: false,
      state: 'error',
      message: 'Unable to load Browser Stream status.',
      codecs: null,
    }
  }
}

async function refreshApps() {
  appsLoading.value = true
  try {
    const response = await fetch('./api/apps', { credentials: 'include' })
    if (!response.ok) {
      throw new Error('apps request failed')
    }
    const body = await response.json()
    apps.value = Array.isArray(body.apps)
      ? body.apps.filter((app) => app && typeof app.uuid === 'string' && app.uuid)
      : []
    const selectedStillExists = apps.value.some((app) => app.uuid === selectedAppUuid.value)
    if (!selectedStillExists) {
      selectedAppUuid.value = typeof body.current_app === 'string' && apps.value.some((app) => app.uuid === body.current_app)
        ? body.current_app
        : ''
    }
  } catch {
    apps.value = []
    selectedAppUuid.value = ''
  } finally {
    appsLoading.value = false
  }
}

function refreshAll() {
  refresh()
  refreshApps()
}

async function start() {
  connecting.value = true
  sessionMessage.value = ''
  resetStats()
  let startedSession = null
  try {
    const response = await fetch('./api/browser-stream/session/start', {
      method: 'POST',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        app_uuid: selectedAppUuid.value,
        stream_profile: selectedProfile.value.id,
        client: {
          screen_width: window.screen?.width || 0,
          screen_height: window.screen?.height || 0,
          device_pixel_ratio: window.devicePixelRatio || 1,
        },
      }),
    })
    const body = await response.json().catch(() => ({}))
    if (!response.ok || body.status === false) {
      throw new Error(body.error || 'Browser Stream could not start.')
    }
    startedSession = body
    await configureVideoDecoder(body)
    await configureAudioDecoder(body)
    const hashes = Array.isArray(body.server_certificate_hashes)
      ? body.server_certificate_hashes
        .filter((hash) => hash?.algorithm === 'sha-256' && hash?.value)
        .map((hash) => ({ algorithm: 'sha-256', value: certHashHexToBytes(hash.value) }))
      : []
    transport = new WebTransport(body.webtransport_url, {
      congestionControl: 'low-latency',
      requireUnreliable: true,
      serverCertificateHashes: hashes,
    })
    await transport.ready
    await openControlStream()
    datagramReader = transport.datagrams.readable.getReader()
    readDatagrams()
    session.value = body
    window.sessionStorage.setItem('browser-stream-app-uuid', selectedAppUuid.value)
    window.sessionStorage.setItem('browser-stream-profile-id', selectedProfile.value.id)
    markStatusReadyFromSession(body)
    sessionMessage.value = 'Browser Stream transport connected.'
  } catch (error) {
    await closeTransport()
    resetVideoPipeline()
    resetAudioPipeline()
    await stopBackendSession(startedSession?.session_token)
    session.value = null
    sessionMessage.value = error instanceof Error ? error.message : 'Browser Stream could not start.'
  } finally {
    connecting.value = false
  }
}

async function stop() {
  if (!session.value) return
  if (gameModeActive.value) {
    await exitGameMode()
  }
  const active = session.value
  session.value = null
  await stopBackendSession(active.session_token)
  await closeTransport()
  resetVideoPipeline()
  resetAudioPipeline()
  try {
    await refresh()
  } finally {
    sessionMessage.value = 'Browser Stream session stopped.'
  }
}

async function readDatagrams() {
  const reader = datagramReader
  if (!reader) return
  try {
    while (reader === datagramReader) {
      const { done, value } = await reader.read()
      if (done) break
      try {
        const datagram = decodeBrowserStreamDatagram(value)
        if (datagram.kind === BrowserStreamKind.video || datagram.kind === BrowserStreamKind.audio) {
          handleMediaDatagram(datagram)
        }
      } catch {
        stats.droppedFrames += 1
      }
    }
  } catch {
    if (reader === datagramReader) {
      sessionMessage.value = 'Browser Stream transport closed.'
    }
  }
}

onMounted(() => {
  document.addEventListener('fullscreenchange', updateGameModeState)
  refreshAll()
})
onBeforeUnmount(() => {
  document.removeEventListener('fullscreenchange', updateGameModeState)
  clearGameModeControlsTimer()
  exitGameMode()
  stop()
})
</script>
