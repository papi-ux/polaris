<script setup>
import { ref, onMounted, onUnmounted, inject } from 'vue'
import { useToast } from '../composables/useToast'

const config = ref({})
const pendingKey = ref(null)
const i18n = inject('i18n')
const { toast } = useToast()
const emit = defineEmits(['change'])
const props = defineProps({
  compact: {
    type: Boolean,
    default: false,
  },
})

// Defaults for settings not explicitly in the config file
const defaults = {
  headless_mode: 'disabled',
  linux_use_cage_compositor: 'disabled',
  adaptive_bitrate_enabled: 'disabled',
  ai_enabled: 'disabled',
  stream_audio: 'enabled',
  enable_discovery: 'enabled',
  enable_pairing: 'enabled',
}

const responseOnlyConfigKeys = [
  'status',
  'platform',
  'version',
  'has_ai_api_key',
  'has_steamgriddb_api_key',
  'has_api_key',
  'vdisplayStatus',
  'vdisplayAvailable',
  'vdisplayBackend',
  'runtime_backend',
  'runtime_requested_headless',
  'runtime_effective_headless',
  'runtime_gpu_native_override_active',
]

function isEnabled(key) {
  const val = config.value[key] ?? defaults[key] ?? 'disabled'
  return val === 'enabled' || val === 'true' || val === true
}

function syncKey(key, value) {
  config.value[key] = value
}

function notifyChange(key, value) {
  emit('change', { key, value, enabled: value === 'enabled' || value === 'true' || value === true })
  window.dispatchEvent(new CustomEvent('polaris:quick-control-updated', {
    detail: { key, value, enabled: value === 'enabled' || value === 'true' || value === true }
  }))
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
    const platform = existing.platform || config.value.platform

    // Remove response-only keys injected by getConfig() that aren't real config settings.
    // These come from the server's status probing, not the config file.
    for (const k of responseOnlyConfigKeys) delete existing[k]

    // Merge the toggle change
    existing[key] = newVal
    if (key === 'headless_mode' && platform === 'linux') {
      existing.linux_use_cage_compositor = newVal
    }

    const response = await fetch('./api/config', {
      credentials: 'include',
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(existing)
    })
    if (!response.ok) throw new Error('save-failed')
    syncKey(key, newVal)
    if (key === 'headless_mode' && platform === 'linux') {
      syncKey('linux_use_cage_compositor', newVal)
    }
    notifyChange(key, newVal)
    await loadConfig()
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

function handleExternalQuickControlUpdate(event) {
  const detail = event?.detail
  if (!detail?.key) return
  syncKey(detail.key, detail.value)
}

onMounted(() => {
  loadConfig()
  window.addEventListener('polaris:quick-control-updated', handleExternalQuickControlUpdate)
})

onUnmounted(() => {
  window.removeEventListener('polaris:quick-control-updated', handleExternalQuickControlUpdate)
})

const toggles = [
  { key: 'headless_mode', requiresRestart: true },
  { key: 'adaptive_bitrate_enabled', requiresRestart: false },
  { key: 'ai_enabled', requiresRestart: false },
  { key: 'stream_audio', requiresRestart: true },
  { key: 'enable_discovery', requiresRestart: false },
  { key: 'enable_pairing', requiresRestart: false },
]

const compactToggleKeys = ['headless_mode', 'adaptive_bitrate_enabled', 'ai_enabled', 'stream_audio']
const visibleToggles = () => (
  props.compact
    ? toggles.filter((toggle) => compactToggleKeys.includes(toggle.key))
    : toggles
)
</script>

<template>
  <div class="space-y-2">
    <div class="flex items-start justify-between gap-3" :class="props.compact ? 'mb-1' : ''">
      <div>
        <div class="section-kicker">{{ $t('quick_controls.title') }}</div>
        <div v-if="!props.compact" class="mt-1 text-[11px] text-storm">{{ $t('quick_controls.helper') }}</div>
        <div v-else class="mt-1 text-[11px] text-storm">Fast stream toggles.</div>
      </div>
      <div class="control-chip whitespace-nowrap">{{ $t('quick_controls.live_default_badge') }}</div>
    </div>
    <button
      v-for="t in visibleToggles()"
      :key="t.key"
      type="button"
      class="focus-ring flex w-full items-center justify-between rounded-xl border border-storm/15 bg-deep/35 text-left transition-[background-color,border-color,color,opacity] duration-200 hover:border-storm/30 hover:bg-ice/5"
      :class="[props.compact ? 'px-3 py-2' : 'px-3 py-2.5', { 'opacity-60': pendingKey && pendingKey !== t.key }]"
      :aria-pressed="isEnabled(t.key)"
      @click="toggle(t.key)"
    >
      <div class="min-w-0 pr-3">
        <div :class="props.compact ? 'flex flex-col items-start gap-1' : 'flex items-center gap-2'">
          <div :class="props.compact ? 'text-[13px] font-medium leading-tight text-silver whitespace-normal break-words' : 'truncate text-sm font-medium text-silver'">
            {{ $t(`quick_controls.items.${t.key}.label`) }}
          </div>
          <span v-if="t.requiresRestart" class="control-chip" :class="props.compact ? 'px-1.5 py-0.5 text-[9px] leading-none tracking-[0.14em]' : ''">
            {{ $t('quick_controls.restart_badge') }}
          </span>
        </div>
        <div v-if="!props.compact" class="mt-1 text-[11px] leading-relaxed text-storm">{{ $t(`quick_controls.items.${t.key}.desc`) }}</div>
      </div>
      <div class="relative ml-2 shrink-0 rounded-full transition-[background-color] duration-200"
           :class="[props.compact ? 'h-[18px] w-8' : 'h-5 w-9', isEnabled(t.key) ? 'bg-accent' : 'bg-storm/40']">
        <div class="absolute rounded-full bg-white transition-transform duration-200"
             :class="props.compact ? 'top-[2px] h-3.5 w-3.5' : 'top-0.5 h-4 w-4'"
             :style="props.compact
               ? { transform: isEnabled(t.key) ? 'translateX(14px)' : 'translateX(2px)' }
               : { transform: isEnabled(t.key) ? 'translateX(18px)' : 'translateX(2px)' }"></div>
      </div>
    </button>
    <div v-if="props.compact" class="pt-1 text-[11px] text-storm/90">
      More host controls in <router-link to="/config" class="text-ice no-underline hover:text-ice/80">Settings</router-link>.
    </div>
    <div v-if="pendingKey" class="pt-1 text-center text-[11px] text-storm">{{ $t('quick_controls.saving') }}</div>
  </div>
</template>
