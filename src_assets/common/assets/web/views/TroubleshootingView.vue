<template>
  <h1 class="text-2xl font-bold text-silver my-6">{{ $t('troubleshooting.troubleshooting') }}</h1>

  <!-- Force Close App -->
  <div class="card p-4 my-6">
    <h2 id="close_apps" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.force_close') }}</h2>
    <p class="text-storm mt-2">{{ $t('troubleshooting.force_close_desc') }}</p>
    <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg mt-3" v-if="closeAppStatus === true">
      {{ $t('troubleshooting.force_close_success') }}
    </div>
    <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg mt-3" v-if="closeAppStatus === false">
      {{ $t('troubleshooting.force_close_error') }}
    </div>
    <div class="mt-3">
      <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200 disabled:opacity-50 disabled:cursor-not-allowed" :disabled="closeAppPressed" @click="closeApp">
        {{ $t('troubleshooting.force_close') }}
      </button>
    </div>
  </div>

  <!-- Restart Polaris -->
  <div class="card p-4 my-6">
    <h2 id="restart" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.restart_polaris') }}</h2>
    <p class="text-storm mt-2">{{ $t('troubleshooting.restart_polaris_desc') }}</p>
    <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg mt-3" v-if="serverRestarting">
      {{ $t('troubleshooting.restart_polaris_success') }}
    </div>
    <div class="mt-3">
      <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200 disabled:opacity-50 disabled:cursor-not-allowed" :disabled="serverQuitting || serverRestarting" @click="restart">
        {{ $t('troubleshooting.restart_polaris') }}
      </button>
    </div>
  </div>

  <!-- Quit Polaris -->
  <div class="card p-4 my-6">
    <h2 id="quit" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.quit_polaris') }}</h2>
    <p class="text-storm mt-2">{{ $t('troubleshooting.quit_polaris_desc') }}</p>
    <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg mt-3" v-if="serverQuit">
      {{ $t('troubleshooting.quit_polaris_success') }}
    </div>
    <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg mt-3" v-if="serverQuitting">
      {{ $t('troubleshooting.quit_polaris_success_ongoing') }}
    </div>
    <div class="mt-3">
      <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200 disabled:opacity-50 disabled:cursor-not-allowed" :disabled="serverQuitting || serverRestarting" @click="quit">
        {{ $t('troubleshooting.quit_polaris') }}
      </button>
    </div>
  </div>

  <!-- Reset persistent display device settings -->
  <div class="card p-4 my-6" v-if="platform === 'windows'">
    <h2 id="dd_reset" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.dd_reset') }}</h2>
    <p class="text-storm mt-2 whitespace-pre-line">{{ $t('troubleshooting.dd_reset_desc') }}</p>
    <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg mt-3" v-if="ddResetStatus === true">
      {{ $t('troubleshooting.dd_reset_success') }}
    </div>
    <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg mt-3" v-if="ddResetStatus === false">
      {{ $t('troubleshooting.dd_reset_error') }}
    </div>
    <div class="mt-3">
      <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200 disabled:opacity-50 disabled:cursor-not-allowed" :disabled="ddResetPressed" @click="ddResetPersistence">
        {{ $t('troubleshooting.dd_reset') }}
      </button>
    </div>
  </div>

  <!-- Logs -->
  <div class="card p-4 my-6">
    <h2 id="logs" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.logs') }}</h2>
    <div class="flex flex-wrap justify-between items-center py-3 gap-3">
      <div class="flex items-center gap-2">
        <button v-for="level in ['All', 'Error', 'Warning', 'Fatal']" :key="level"
                @click="logLevelFilter = level === 'All' ? null : level"
                class="h-7 px-2.5 text-xs font-medium rounded-md transition-all duration-150"
                :class="(logLevelFilter === level || (level === 'All' && !logLevelFilter))
                  ? 'bg-ice/15 text-ice border border-ice/30'
                  : 'text-storm border border-storm/30 hover:border-storm hover:text-silver'">
          {{ level }}
        </button>
      </div>
      <input type="text" class="w-60 bg-deep border border-storm/50 rounded-lg px-3 py-1.5 text-sm text-silver focus:border-ice focus:outline-none shrink-0" v-model="logFilter" :placeholder="$t('troubleshooting.logs_find')">
    </div>
    <VirtualLogViewer :logs="actualLogs" :containerHeight="400" @copy="copyLogs" />
  </div>
</template>

<script setup>
import { ref, computed, onBeforeUnmount, inject } from 'vue'
import { useToast } from '../composables/useToast'
import VirtualLogViewer from '../components/VirtualLogViewer.vue'

const { toast: showToast } = useToast()
const i18n = inject('i18n')

const clients = ref([])
const closeAppPressed = ref(false)
const closeAppStatus = ref(null)
const ddResetPressed = ref(false)
const ddResetStatus = ref(null)
const logs = ref('Loading...')
const logFilter = ref(null)
const logLevelFilter = ref(null)
let logInterval = null
const serverRestarting = ref(false)
const serverQuitting = ref(false)
const serverQuit = ref(false)
const platform = ref("")

const actualLogs = computed(() => {
  if (!logFilter.value && !logLevelFilter.value) return logs.value
  let lines = logs.value.split("\n")
  if (logLevelFilter.value) {
    lines = lines.filter(x => x.includes(logLevelFilter.value + ':'))
  }
  if (logFilter.value) {
    lines = lines.filter(x => x.indexOf(logFilter.value) !== -1)
  }
  return lines.join("\n")
})

function refreshLogs() {
  fetch("./api/logs", { credentials: 'include' })
    .then(response => {
      const contentType = response.headers.get("Content-Type") || ""
      const charsetMatch = contentType.match(/charset=([^;]+)/i)
      const charset = charsetMatch ? charsetMatch[1].trim() : "utf-8"
      return response.arrayBuffer().then(buffer => {
        const decoder = new TextDecoder(charset)
        return decoder.decode(buffer)
      })
    })
    .then(text => {
      logs.value = text
    })
    .catch(error => console.error("Error fetching logs:", error))
}

function closeApp() {
  closeAppPressed.value = true
  fetch("./api/apps/close", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      closeAppPressed.value = false
      closeAppStatus.value = r.status
      setTimeout(() => { closeAppStatus.value = null }, 5000)
    })
}

function copyLogs() {
  navigator.clipboard.writeText(actualLogs.value)
}

function restart() {
  serverRestarting.value = true
  setTimeout(() => { serverRestarting.value = false }, 5000)
  fetch("./api/restart", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
  .then((resp) => {
    if (resp.status !== 200) {
      setTimeout(() => { location.reload() }, 1000)
      return
    }
  })
  .catch((e) => {
    serverRestarting.value = false
    console.error(e)
    setTimeout(() => { location.reload() }, 1000)
  })
}

function quit() {
  if (window.confirm(i18n.t('troubleshooting.quit_polaris_confirm'))) {
    serverQuitting.value = true
    fetch("./api/quit", {
      credentials: 'include',
      method: 'POST',
      headers: { 'Content-Type': 'application/json' }
    })
    .then(() => {
      serverQuitting.value = false
      serverQuit.value = false
      showToast("Exit failed!", 'error')
    })
    .catch(() => {
      serverQuitting.value = false
      serverQuit.value = true
    })
  }
}

function ddResetPersistence() {
  ddResetPressed.value = true
  fetch("/api/reset-display-device-persistence", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      ddResetPressed.value = false
      ddResetStatus.value = r.status
      setTimeout(() => { ddResetStatus.value = null }, 5000)
    })
}

// created() logic
fetch("/api/config")
  .then((r) => r.json())
  .then((r) => { platform.value = r.platform })

logInterval = setInterval(() => { refreshLogs() }, 5000)
refreshLogs()

onBeforeUnmount(() => {
  clearInterval(logInterval)
})
</script>

<style scoped>
.troubleshooting-logs {
  white-space: pre;
  font-family: monospace;
  overflow: auto;
  max-height: 500px;
  min-height: 500px;
  font-size: 14px;
  position: relative;
  background-color: #2a2840;
  border-radius: 0.5rem;
  padding: 1rem;
  color: #979f9e;
}

.copy-icon {
  position: absolute;
  top: 8px;
  right: 8px;
  padding: 8px;
  cursor: pointer;
  color: #687b81;
  appearance: none;
  border: none;
  background: none;
  transition: color 0.15s;
}

.copy-icon:hover {
  color: #c8d6e5;
}
</style>
