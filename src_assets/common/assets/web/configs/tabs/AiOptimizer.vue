<script setup>
import { computed, onMounted, ref, watch } from 'vue'
import { useAiOptimizer } from '../../composables/useAiOptimizer'
import { useToast } from '../../composables/useToast'

const props = defineProps(['config'])
const config = ref(props.config)

const { toast } = useToast()
const {
  status: aiStatus, cache: aiCache, devices: aiDevices, loading: aiLoading,
  fetchStatus, fetchCache, fetchDevices, clearCache, testConnection
} = useAiOptimizer()

const showApiKey = ref(false)
const testResult = ref(null)
const testLoading = ref(false)
const deviceSearch = ref('')
const cacheExpanded = ref(false)
const filteredDevices = ref([])

const providerOptions = [
  {
    id: 'anthropic',
    name: 'Claude',
    eyebrow: 'Anthropic',
    summary: 'Keep the existing Claude path, with direct API access or a local Claude subscription session.',
    defaultModel: 'claude-haiku-4-5-20251001',
    defaultBaseUrl: 'https://api.anthropic.com',
    defaultAuth: 'subscription',
    authModes: ['subscription', 'api_key'],
    accent: 'border-amber-300/30 bg-amber-300/8 text-amber-200',
    pill: 'text-amber-200 border-amber-300/30',
    keyPlaceholder: 'sk-ant-api03-...',
    keyHint: 'Anthropic API key from console.anthropic.com.',
  },
  {
    id: 'openai',
    name: 'OpenAI',
    eyebrow: 'Responses via compatibility',
    summary: 'Use an OpenAI-compatible chat completion flow with structured JSON output for optimization results.',
    defaultModel: 'gpt-5.4-mini',
    defaultBaseUrl: 'https://api.openai.com/v1',
    defaultAuth: 'api_key',
    authModes: ['api_key'],
    accent: 'border-emerald-300/30 bg-emerald-300/8 text-emerald-200',
    pill: 'text-emerald-200 border-emerald-300/30',
    keyPlaceholder: 'sk-proj-...',
    keyHint: 'OpenAI API key from platform.openai.com.',
  },
  {
    id: 'gemini',
    name: 'Gemini',
    eyebrow: 'Google AI',
    summary: 'Route through Gemini\'s OpenAI-compatible endpoint so provider behavior stays aligned with the shared backend.',
    defaultModel: 'gemini-2.5-flash',
    defaultBaseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
    defaultAuth: 'api_key',
    authModes: ['api_key'],
    accent: 'border-sky-300/30 bg-sky-300/8 text-sky-200',
    pill: 'text-sky-200 border-sky-300/30',
    keyPlaceholder: 'AIza...',
    keyHint: 'Gemini API key from Google AI Studio.',
  },
  {
    id: 'local',
    name: 'Local',
    eyebrow: 'Ollama / LM Studio',
    summary: 'Point Polaris at a local OpenAI-compatible server so users can keep optimization fully on-box.',
    defaultModel: 'gpt-oss',
    defaultBaseUrl: 'http://127.0.0.1:11434/v1',
    defaultAuth: 'none',
    authModes: ['none', 'api_key'],
    accent: 'border-stone-300/25 bg-stone-300/8 text-stone-200',
    pill: 'text-stone-200 border-stone-300/25',
    keyPlaceholder: 'optional',
    keyHint: 'Usually not required for local endpoints. LM Studio or reverse proxies may add one.',
  }
]

const authModeLabels = {
  subscription: { name: 'Subscription', description: 'Use a local Claude CLI session' },
  api_key: { name: 'API Key', description: 'Authenticate directly to the provider endpoint' },
  none: { name: 'No Auth', description: 'Use an unauthenticated local endpoint' }
}

const currentProvider = computed(() =>
  providerOptions.find(provider => provider.id === config.value.ai_provider) || providerOptions[0]
)

const liveProvider = computed(() =>
  providerOptions.find(provider => provider.id === aiStatus.value?.provider) || null
)

const availableAuthModes = computed(() => currentProvider.value.authModes)

const canTestDraft = computed(() => {
  if (config.value.ai_auth_mode === 'subscription') return true
  if (config.value.ai_auth_mode === 'none') return true
  return !!config.value.ai_api_key
})

function providerPill(providerId) {
  const provider = providerOptions.find(item => item.id === providerId)
  return provider?.pill || 'text-silver border-storm/40'
}

function syncProviderDefaults(previousProviderId) {
  const provider = currentProvider.value
  const previousProvider = providerOptions.find(item => item.id === previousProviderId)

  if (!config.value.ai_provider) {
    config.value.ai_provider = provider.id
  }

  if (!config.value.ai_model || (previousProvider && config.value.ai_model === previousProvider.defaultModel)) {
    config.value.ai_model = provider.defaultModel
  }

  if (!config.value.ai_base_url || (previousProvider && config.value.ai_base_url === previousProvider.defaultBaseUrl)) {
    config.value.ai_base_url = provider.defaultBaseUrl
  }

  if (!availableAuthModes.value.includes(config.value.ai_auth_mode)) {
    if (provider.id === 'anthropic' && config.value.ai_use_subscription === 'enabled') {
      config.value.ai_auth_mode = 'subscription'
    } else {
      config.value.ai_auth_mode = provider.defaultAuth
    }
  }

  config.value.ai_use_subscription = config.value.ai_auth_mode === 'subscription' ? 'enabled' : 'disabled'

  if (config.value.ai_auth_mode === 'none') {
    config.value.ai_api_key = ''
  }
}

watch(() => config.value.ai_provider, (nextProvider, previousProvider) => {
  syncProviderDefaults(previousProvider)
}, { immediate: true })

watch(() => config.value.ai_auth_mode, (nextMode) => {
  config.value.ai_use_subscription = nextMode === 'subscription' ? 'enabled' : 'disabled'
  if (nextMode === 'none') {
    config.value.ai_api_key = ''
  }
})

function buildDraftPayload() {
  return {
    ai_provider: config.value.ai_provider,
    ai_model: config.value.ai_model,
    ai_auth_mode: config.value.ai_auth_mode,
    ai_api_key: config.value.ai_api_key,
    ai_base_url: config.value.ai_base_url,
    ai_use_subscription: config.value.ai_use_subscription,
    ai_timeout_ms: Number(config.value.ai_timeout_ms) || 5000,
    ai_cache_ttl_hours: Number(config.value.ai_cache_ttl_hours) || 168
  }
}

async function testProviderConfig() {
  testLoading.value = true
  testResult.value = null

  const provider = currentProvider.value
  const result = await testConnection(buildDraftPayload(), 'Test Device', 'Test App')

  testLoading.value = false
  if (result.status) {
    const message = provider.id === 'local'
      ? 'Local endpoint responded with a valid optimization payload.'
      : `${provider.name} responded with a valid optimization payload.`
    testResult.value = { success: true, message, detail: result.reasoning || '' }
    toast(`${provider.name} draft settings verified`, 'success')
  } else {
    testResult.value = {
      success: false,
      message: result.error || 'Connection test failed',
      detail: ''
    }
    toast(`${provider.name} test failed: ${result.error || 'Unknown error'}`, 'error')
  }
}

async function handleClearCache() {
  const ok = await clearCache()
  if (ok) {
    toast('AI optimization cache cleared', 'success')
    fetchCache()
  } else {
    toast('Failed to clear cache', 'error')
  }
}

function filterDevices() {
  const query = deviceSearch.value.toLowerCase()
  if (!Array.isArray(aiDevices.value)) {
    filteredDevices.value = []
    return
  }

  if (!query) {
    filteredDevices.value = aiDevices.value
    return
  }

  filteredDevices.value = aiDevices.value.filter(device =>
    (device.name || '').toLowerCase().includes(query) ||
    (device.type || '').toLowerCase().includes(query)
  )
}

onMounted(async () => {
  await Promise.all([fetchStatus(), fetchCache(), fetchDevices()])
  filterDevices()
})
</script>

<template>
  <div class="config-page space-y-6">
    <div class="glass rounded-2xl border border-storm/40 p-5 space-y-4">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
        <div class="max-w-3xl">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-[0.28em]">Optimization Studio</div>
          <h2 class="text-xl font-semibold text-silver mt-2">Choose the model provider Polaris should use for device tuning.</h2>
          <p class="text-sm text-storm mt-2">
            The optimizer can now target Claude, OpenAI, Gemini, or a local OpenAI-compatible endpoint. Draft connection tests use the unsaved settings below; save and apply when you want live sessions to use them.
          </p>
        </div>

        <div class="flex items-center gap-3 rounded-2xl border border-storm/40 bg-void/30 px-4 py-3">
          <div>
            <div class="text-xs uppercase tracking-wider text-storm">Runtime AI</div>
            <div class="text-sm font-medium text-silver mt-1">
              {{ config.ai_enabled === 'enabled' ? 'Enabled' : 'Disabled' }}
            </div>
          </div>
          <button
            @click="config.ai_enabled = config.ai_enabled === 'enabled' ? 'disabled' : 'enabled'"
            class="relative inline-flex h-6 w-11 shrink-0 cursor-pointer rounded-full transition-colors duration-200"
            :class="config.ai_enabled === 'enabled' ? 'bg-ice' : 'bg-storm/50'">
            <span
              class="inline-block h-5 w-5 transform rounded-full bg-white shadow-lg transition-transform duration-200 mt-0.5"
              :class="config.ai_enabled === 'enabled' ? 'translate-x-[22px]' : 'translate-x-0.5'"></span>
          </button>
        </div>
      </div>

      <div class="grid gap-3 lg:grid-cols-4">
        <button
          v-for="provider in providerOptions"
          :key="provider.id"
          @click="config.ai_provider = provider.id"
          class="rounded-2xl border p-4 text-left transition-all duration-200"
          :class="config.ai_provider === provider.id ? provider.accent : 'border-storm/40 bg-deep hover:border-ice/40 hover:bg-twilight/30'">
          <div class="flex items-center justify-between gap-3">
            <div>
              <div class="text-[10px] uppercase tracking-[0.22em]" :class="config.ai_provider === provider.id ? 'text-current/80' : 'text-storm'">{{ provider.eyebrow }}</div>
              <div class="text-base font-semibold mt-1" :class="config.ai_provider === provider.id ? 'text-current' : 'text-silver'">{{ provider.name }}</div>
            </div>
            <span
              class="h-2.5 w-2.5 rounded-full"
              :class="config.ai_provider === provider.id ? 'bg-current' : 'bg-storm/50'"></span>
          </div>
          <p class="text-sm mt-3 leading-6" :class="config.ai_provider === provider.id ? 'text-current/90' : 'text-storm'">
            {{ provider.summary }}
          </p>
        </button>
      </div>
    </div>

    <div class="grid gap-6 xl:grid-cols-[1.4fr_0.9fr]">
      <div class="space-y-6">
        <div class="rounded-2xl border border-storm/40 bg-deep p-5 space-y-5">
          <div class="flex items-start justify-between gap-4">
            <div>
              <div class="text-[10px] font-semibold text-storm uppercase tracking-[0.24em]">Draft Connection</div>
              <h3 class="text-lg font-semibold text-silver mt-2">{{ currentProvider.name }} setup</h3>
              <p class="text-sm text-storm mt-1">
                Model choice, authentication, and endpoint are all provider-specific. Polaris will resolve sensible defaults if you keep the standard values.
              </p>
            </div>
            <span class="inline-flex items-center rounded-full border px-2.5 py-1 text-[11px] font-medium" :class="providerPill(currentProvider.id)">
              {{ currentProvider.name }}
            </span>
          </div>

          <div class="space-y-3">
            <div class="text-xs font-semibold uppercase tracking-[0.2em] text-storm">Authentication</div>
            <div class="grid gap-2 sm:grid-cols-2">
              <button
                v-for="mode in availableAuthModes"
                :key="mode"
                @click="config.ai_auth_mode = mode"
                class="rounded-xl border px-4 py-3 text-left transition-all duration-200"
                :class="config.ai_auth_mode === mode ? 'border-ice/40 bg-ice/10 text-ice' : 'border-storm/40 bg-void/30 text-silver hover:border-ice/30'">
                <div class="text-sm font-medium">{{ authModeLabels[mode].name }}</div>
                <div class="text-xs text-storm mt-1">{{ authModeLabels[mode].description }}</div>
              </button>
            </div>
          </div>

          <div class="grid gap-4 lg:grid-cols-2">
            <div>
              <label class="block text-sm font-medium text-silver mb-1">Model</label>
              <input
                v-model="config.ai_model"
                type="text"
                class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm"
                :placeholder="currentProvider.defaultModel" />
              <div class="text-xs text-storm mt-1">Default for {{ currentProvider.name }}: <span class="font-mono text-silver/80">{{ currentProvider.defaultModel }}</span></div>
            </div>

            <div>
              <label class="block text-sm font-medium text-silver mb-1">Base URL</label>
              <input
                v-model="config.ai_base_url"
                type="text"
                class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm"
                :placeholder="currentProvider.defaultBaseUrl" />
              <div class="text-xs text-storm mt-1">Default endpoint: <span class="font-mono text-silver/80">{{ currentProvider.defaultBaseUrl }}</span></div>
            </div>
          </div>

          <div v-if="config.ai_auth_mode === 'subscription'" class="rounded-xl border border-amber-300/20 bg-amber-300/6 p-4">
            <div class="text-sm text-silver">Polaris will call the local <code class="bg-void/40 px-1 rounded text-amber-200">claude</code> CLI instead of a remote API key flow.</div>
            <div class="text-xs text-storm mt-2">Use this only for Claude. The Web UI can test the draft config below, but live sessions still switch over only after save and apply.</div>
          </div>

          <div v-else-if="config.ai_auth_mode === 'none'" class="rounded-xl border border-stone-300/20 bg-stone-300/6 p-4">
            <div class="text-sm text-silver">Polaris will call the configured endpoint without an Authorization header.</div>
            <div class="text-xs text-storm mt-2">This is the usual setup for Ollama on <code class="bg-void/40 px-1 rounded">http://127.0.0.1:11434/v1</code>.</div>
          </div>

          <div v-else>
            <label class="block text-sm font-medium text-silver mb-1">API Key</label>
            <div class="flex gap-2">
              <div class="relative flex-1">
                <input
                  :type="showApiKey ? 'text' : 'password'"
                  v-model="config.ai_api_key"
                  class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2 pr-10 text-silver focus:border-ice focus:outline-none font-mono text-sm"
                  :placeholder="currentProvider.keyPlaceholder" />
                <button @click="showApiKey = !showApiKey" class="absolute right-2 top-1/2 -translate-y-1/2 text-storm hover:text-silver transition-colors">
                  <svg v-if="!showApiKey" class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z"/></svg>
                  <svg v-else class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13.875 18.825A10.05 10.05 0 0112 19c-4.478 0-8.268-2.943-9.543-7a9.97 9.97 0 011.563-3.029m5.858.908a3 3 0 114.243 4.243M9.878 9.878l4.242 4.242M9.88 9.88l-3.29-3.29m7.532 7.532l3.29 3.29M3 3l3.59 3.59m0 0A9.953 9.953 0 0112 5c4.478 0 8.268 2.943 9.543 7a10.025 10.025 0 01-4.132 5.411m0 0L21 21"/></svg>
                </button>
              </div>
            </div>
            <div class="text-xs text-storm mt-1">{{ currentProvider.keyHint }}</div>
          </div>
        </div>

        <div class="rounded-2xl border border-storm/40 bg-deep p-5 space-y-4">
          <div class="flex items-center justify-between gap-3">
            <div>
              <div class="text-[10px] font-semibold text-storm uppercase tracking-[0.24em]">Response Policy</div>
              <div class="text-base font-semibold text-silver mt-2">Cache and timeout</div>
              <div class="text-sm text-storm mt-1">These settings apply to all providers, including local endpoints.</div>
            </div>
            <button
              @click="testProviderConfig"
              :disabled="testLoading || aiLoading || !canTestDraft"
              class="inline-flex items-center gap-2 h-10 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 transition-all duration-200 disabled:opacity-50 disabled:cursor-not-allowed">
              <svg v-if="testLoading || aiLoading" class="w-4 h-4 animate-spin" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
              <span>{{ testLoading || aiLoading ? 'Testing…' : 'Test Draft Connection' }}</span>
            </button>
          </div>

          <div class="grid gap-4 lg:grid-cols-2">
            <div>
              <label class="block text-sm font-medium text-silver mb-1">Cache Duration</label>
              <div class="flex items-center gap-3">
                <input
                  v-model="config.ai_cache_ttl_hours"
                  type="range"
                  min="24"
                  max="720"
                  step="24"
                  class="flex-1 accent-ice h-1.5 rounded-lg cursor-pointer" />
                <span class="text-sm text-silver font-medium w-12 text-right">{{ config.ai_cache_ttl_hours ? Math.round(config.ai_cache_ttl_hours / 24) + 'd' : '7d' }}</span>
              </div>
            </div>

            <div>
              <label class="block text-sm font-medium text-silver mb-1">Timeout</label>
              <div class="flex items-center gap-2">
                <input
                  v-model="config.ai_timeout_ms"
                  type="number"
                  min="1000"
                  max="30000"
                  step="500"
                  class="w-32 bg-void/50 border border-storm/50 rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
                  placeholder="5000" />
                <span class="text-sm text-storm">ms</span>
              </div>
            </div>
          </div>

          <div v-if="testResult" class="rounded-xl border px-4 py-3" :class="testResult.success ? 'border-green-400/20 bg-green-400/8' : 'border-red-400/20 bg-red-400/8'">
            <div class="text-sm font-medium" :class="testResult.success ? 'text-green-300' : 'text-red-300'">{{ testResult.message }}</div>
            <div v-if="testResult.detail" class="text-xs text-silver/70 mt-2">{{ testResult.detail }}</div>
          </div>
        </div>
      </div>

      <div class="space-y-6">
        <div class="rounded-2xl border border-storm/40 bg-deep p-5 space-y-4">
          <div>
            <div class="text-[10px] font-semibold text-storm uppercase tracking-[0.24em]">Live Runtime</div>
            <div class="text-base font-semibold text-silver mt-2">Saved optimizer status</div>
            <div class="text-sm text-storm mt-1">This reflects the currently loaded runtime config, not the unsaved draft on the left.</div>
          </div>

          <div class="grid gap-3">
            <div class="rounded-xl border border-storm/30 bg-void/30 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">Provider</div>
              <div class="flex items-center gap-2 mt-2">
                <span v-if="liveProvider" class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="providerPill(liveProvider.id)">
                  {{ liveProvider.name }}
                </span>
                <span class="text-sm text-silver">{{ aiStatus?.provider || 'Not loaded' }}</span>
              </div>
            </div>

            <div class="rounded-xl border border-storm/30 bg-void/30 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">Model</div>
              <div class="text-sm text-silver font-mono mt-2 break-all">{{ aiStatus?.model || 'Not loaded' }}</div>
            </div>

            <div class="rounded-xl border border-storm/30 bg-void/30 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">Endpoint</div>
              <div class="text-sm text-silver font-mono mt-2 break-all">{{ aiStatus?.base_url || 'Not loaded' }}</div>
            </div>

            <div class="grid gap-3 sm:grid-cols-2">
              <div class="rounded-xl border border-storm/30 bg-void/30 p-3">
                <div class="text-xs uppercase tracking-wider text-storm">Auth</div>
                <div class="text-sm text-silver mt-2">{{ aiStatus?.auth_mode || 'Unknown' }}</div>
              </div>

              <div class="rounded-xl border border-storm/30 bg-void/30 p-3">
                <div class="text-xs uppercase tracking-wider text-storm">Cache Entries</div>
                <div class="text-sm text-silver mt-2">{{ aiStatus?.cache_count ?? 0 }}</div>
              </div>
            </div>

            <div v-if="aiStatus?.auth_mode === 'subscription'" class="rounded-xl border border-amber-300/20 bg-amber-300/6 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">Claude CLI</div>
              <div class="text-sm mt-2" :class="aiStatus?.cli_available ? 'text-green-300' : 'text-red-300'">
                {{ aiStatus?.cli_available ? 'Detected in PATH' : 'Not found in PATH' }}
              </div>
            </div>
          </div>
        </div>

        <div class="p-4 bg-deep rounded-2xl border border-storm/40">
          <div class="flex items-center justify-between mb-3 cursor-pointer" @click="cacheExpanded = !cacheExpanded">
            <div class="flex items-center gap-2">
              <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider">Optimization Cache</div>
              <span class="px-1.5 py-0.5 rounded text-xs font-mono bg-twilight text-silver">{{ Array.isArray(aiCache) ? aiCache.length : 0 }}</span>
            </div>
            <div class="flex items-center gap-2">
              <button
                @click.stop="handleClearCache"
                class="text-xs text-storm hover:text-red-400 transition-colors"
                :class="{ 'opacity-50 pointer-events-none': !Array.isArray(aiCache) || aiCache.length === 0 }">
                Clear All
              </button>
              <svg class="w-4 h-4 text-storm transition-transform" :class="{ 'rotate-180': cacheExpanded }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
            </div>
          </div>

          <div v-if="cacheExpanded && Array.isArray(aiCache) && aiCache.length > 0" class="space-y-2 max-h-96 overflow-y-auto scrollbar-hidden">
            <div v-for="(entry, i) in aiCache" :key="i" class="py-2" :class="i > 0 ? 'border-t border-storm/20' : ''">
              <div class="flex items-center justify-between text-sm cursor-pointer" @click="entry._expanded = !entry._expanded">
                <div class="min-w-0 flex-1">
                  <div class="text-silver font-medium truncate">{{ entry.device_name }}{{ entry.app_name ? ' + ' + entry.app_name : '' }}</div>
                  <div class="text-xs text-silver/60">
                    {{ entry.display_mode || '—' }} · {{ entry.target_bitrate_kbps ? (entry.target_bitrate_kbps / 1000).toFixed(0) + ' Mbps' : '—' }}
                    <span v-if="entry.preferred_codec"> · {{ entry.preferred_codec.toUpperCase() }}</span>
                    <span v-if="entry.model"> · {{ entry.model }}</span>
                  </div>
                </div>
                <div class="flex items-center gap-2 shrink-0 ml-3">
                  <div class="text-xs text-storm" v-if="entry.cached_at">
                    {{ new Date(entry.cached_at * 1000).toLocaleDateString([], { month: 'short', day: 'numeric' }) }}
                  </div>
                  <svg class="w-3 h-3 text-storm transition-transform" :class="{ 'rotate-180': entry._expanded }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
                </div>
              </div>
              <div v-if="entry._expanded" class="mt-2 p-3 bg-void/50 rounded-lg text-xs space-y-1.5">
                <div class="flex justify-between" v-if="entry.display_mode">
                  <span class="text-storm">Display Mode</span>
                  <span class="text-ice font-mono">{{ entry.display_mode }}</span>
                </div>
                <div class="flex justify-between" v-if="entry.target_bitrate_kbps">
                  <span class="text-storm">Target Bitrate</span>
                  <span class="text-ice font-mono">{{ (entry.target_bitrate_kbps / 1000).toFixed(1) }} Mbps</span>
                </div>
                <div class="flex justify-between" v-if="entry.nvenc_tune">
                  <span class="text-storm">NVENC Tune</span>
                  <span class="text-ice font-mono">{{ entry.nvenc_tune === 1 ? 'Quality' : entry.nvenc_tune === 2 ? 'Low Latency' : 'Ultra Low Latency' }}</span>
                </div>
                <div class="flex justify-between" v-if="entry.color_range != null">
                  <span class="text-storm">Color Range</span>
                  <span class="text-ice font-mono">{{ entry.color_range === 0 ? 'Client' : entry.color_range === 1 ? 'Limited' : 'Full' }}</span>
                </div>
                <div class="flex justify-between" v-if="entry.preferred_codec">
                  <span class="text-storm">Preferred Codec</span>
                  <span class="text-ice font-mono">{{ entry.preferred_codec.toUpperCase() }}</span>
                </div>
                <div class="flex justify-between" v-if="entry.hdr != null">
                  <span class="text-storm">HDR</span>
                  <span class="font-mono" :class="entry.hdr ? 'text-green-400' : 'text-storm'">{{ entry.hdr ? 'Yes' : 'No' }}</span>
                </div>
                <div v-if="entry.reasoning" class="pt-1.5 border-t border-storm/20">
                  <span class="text-storm">AI Reasoning: </span>
                  <span class="text-silver/80 italic">{{ entry.reasoning }}</span>
                </div>
              </div>
            </div>
          </div>
          <div v-else-if="cacheExpanded" class="text-sm text-storm text-center py-3">No cached optimizations yet</div>
        </div>
      </div>
    </div>

    <div class="p-4 bg-deep rounded-2xl border border-storm/40">
      <div class="flex items-center justify-between mb-3">
        <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider">Device Knowledge Base</div>
        <span class="px-1.5 py-0.5 rounded text-xs font-mono bg-twilight text-silver">{{ filteredDevices.length }} devices</span>
      </div>
      <input
        v-model="deviceSearch"
        @input="filterDevices"
        type="text"
        placeholder="Search devices..."
        class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2 text-sm text-silver focus:border-ice focus:outline-none mb-3" />
      <div class="space-y-1 max-h-72 overflow-y-auto scrollbar-hidden">
        <div v-for="device in filteredDevices" :key="device.name" class="flex items-center justify-between py-2 px-2 rounded-lg hover:bg-twilight/30 transition-colors">
          <div class="min-w-0 flex-1">
            <div class="text-sm text-silver font-medium">{{ device.name }}</div>
            <div class="text-xs text-silver/60">
              <span class="capitalize">{{ device.type }}</span>
              <span v-if="device.display_mode"> · {{ device.display_mode }}</span>
              <span v-if="device.preferred_codec"> · {{ device.preferred_codec.toUpperCase() }}</span>
              <span v-if="device.ideal_bitrate_kbps"> · {{ (device.ideal_bitrate_kbps / 1000).toFixed(0) }} Mbps</span>
              <span v-if="device.hdr_capable" class="text-ice"> HDR</span>
            </div>
          </div>
          <div class="flex items-center gap-1 shrink-0">
            <span class="px-1.5 py-0.5 rounded text-xs" :class="{
              'bg-green-500/10 text-green-400': device.type === 'handheld',
              'bg-blue-500/10 text-blue-400': device.type === 'phone',
              'bg-yellow-500/10 text-yellow-400': device.type === 'desktop',
              'bg-orange-500/10 text-orange-400': device.type === 'tablet'
            }">{{ device.type }}</span>
          </div>
        </div>
        <div v-if="filteredDevices.length === 0" class="text-sm text-storm text-center py-3">
          {{ deviceSearch ? 'No matching devices' : 'Loading devices...' }}
        </div>
      </div>
    </div>
  </div>
</template>
