<script setup>
import { ref, onMounted, inject } from 'vue'
import { useToast } from '../composables/useToast'

const config = ref({})
const pendingKey = ref(null)
const i18n = inject('i18n')
const { toast } = useToast()

// Defaults for settings not explicitly in the config file
const defaults = {
  headless_mode: 'disabled',
  adaptive_bitrate_enabled: 'disabled',
  ai_enabled: 'disabled',
  stream_audio: 'enabled',
  enable_discovery: 'enabled',
  enable_pairing: 'enabled',
}

function isEnabled(key) {
  const val = config.value[key] ?? defaults[key] ?? 'disabled'
  return val === 'enabled' || val === 'true' || val === true
}

async function loadConfig() {
  try {
    const res = await fetch('./api/config', { credentials: 'include' })
    if (res.ok) {
      const data = await res.json()
      // Merge with defaults
      config.value = { ...defaults, ...data }
    }
  } catch {}
}

async function toggle(key) {
  if (pendingKey.value) return
  pendingKey.value = key
  const newVal = isEnabled(key) ? 'disabled' : 'enabled'
  try {
    // Read existing config first, then merge the change
    // The API overwrites the entire file, so we must send ALL settings
    const existingRes = await fetch('./api/config', { credentials: 'include' })
    let existing = {}
    if (existingRes.ok) {
      existing = await existingRes.json()
    }
    // Remove runtime-only keys injected by getConfig() that aren't real config settings.
    // These come from the server's status probing, not the config file.
    const runtimeKeys = ['platform', 'status', 'version', 'vdisplayAvailable', 'vdisplayBackend', 'vdisplayStatus']
    for (const k of runtimeKeys) delete existing[k]

    // Merge the toggle change
    existing[key] = newVal

    const response = await fetch('./api/config', {
      credentials: 'include',
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(existing)
    })
    if (!response.ok) throw new Error('save-failed')
    config.value[key] = newVal
    toast(
      `${labelFor(key)} ${newVal === 'enabled' ? i18n.t('quick_controls.enabled_now') : i18n.t('quick_controls.disabled_now')}`,
      'success',
      2600
    )
  } catch (e) {
    console.error('Toggle failed:', e)
    toast(i18n.t('quick_controls.save_failed'), 'error')
  }
  pendingKey.value = null
}

function labelFor(key) {
  return i18n.t(`quick_controls.items.${key}.label`)
}

onMounted(loadConfig)

const toggles = [
  { key: 'headless_mode', requiresRestart: true },
  { key: 'adaptive_bitrate_enabled', requiresRestart: false },
  { key: 'ai_enabled', requiresRestart: false },
  { key: 'stream_audio', requiresRestart: true },
  { key: 'enable_discovery', requiresRestart: false },
  { key: 'enable_pairing', requiresRestart: false },
]
</script>

<template>
  <div class="space-y-1">
    <div class="flex items-center justify-between mb-2">
      <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">{{ $t('quick_controls.title') }}</div>
      <div class="text-[10px] text-storm">{{ $t('quick_controls.helper') }}</div>
    </div>
    <div v-for="t in toggles" :key="t.key"
         class="flex items-center justify-between py-1.5 px-2 rounded-lg hover:bg-ice/5 transition-colors cursor-pointer"
         :class="{ 'opacity-60': pendingKey && pendingKey !== t.key }"
         @click="toggle(t.key)">
      <div class="min-w-0">
        <div class="flex items-center gap-2">
          <div class="text-xs text-silver font-medium truncate">{{ $t(`quick_controls.items.${t.key}.label`) }}</div>
          <span class="px-1.5 py-0.5 rounded text-[9px] border border-storm/30 text-storm">
            {{ t.requiresRestart ? $t('quick_controls.restart_badge') : $t('quick_controls.live_badge') }}
          </span>
        </div>
        <div class="text-[9px] text-storm truncate">{{ $t(`quick_controls.items.${t.key}.desc`) }}</div>
      </div>
      <div class="w-7 h-4 rounded-full transition-colors shrink-0 ml-2 relative"
           :class="isEnabled(t.key) ? 'bg-accent' : 'bg-storm/40'">
        <div class="absolute top-0.5 w-3 h-3 rounded-full bg-white transition-transform"
             :class="isEnabled(t.key) ? 'translate-x-3.5' : 'translate-x-0.5'"></div>
      </div>
    </div>
    <div v-if="pendingKey" class="text-[9px] text-storm text-center mt-1">{{ $t('quick_controls.saving') }}</div>
  </div>
</template>
