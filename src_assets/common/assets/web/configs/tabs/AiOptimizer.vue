<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useAiOptimizer } from '../../composables/useAiOptimizer'
import { useToast } from '../../composables/useToast'

const props = defineProps(['config'])
const config = ref(props.config)

const { toast } = useToast()
const {
  status: aiStatus, cache: aiCache, history: aiHistory, devices: aiDevices, loading: aiLoading,
  modelCatalog, modelsLoading,
  fetchStatus, fetchCache, fetchHistory, fetchDevices, fetchModels, clearCache, testConnection
} = useAiOptimizer()

const showApiKey = ref(false)
const testResult = ref(null)
const testLoading = ref(false)
const testDeviceName = ref('')
const testAppName = ref('')
const deviceSearch = ref('')
const cacheExpanded = ref(false)
const knowledgeExpanded = ref(false)
const filteredDevices = ref([])
let modelRefreshTimer = null

const providerOptions = [
  {
    id: 'anthropic',
    name: 'Claude',
    eyebrow: 'Anthropic',
    summary: 'Claude via CLI subscription or direct API access.',
    defaultModel: 'claude-haiku-4-5-20251001',
    defaultBaseUrl: 'https://api.anthropic.com',
    defaultAuth: 'subscription',
    authModes: ['subscription', 'api_key'],
    accent: 'border-amber-300/30 bg-amber-300/8 text-amber-200',
    pill: 'text-amber-200 border-amber-300/30',
    subscriptionLabel: 'Claude CLI',
    subscriptionBinary: 'claude',
    keyPlaceholder: 'sk-ant-api03-...',
    keyHint: 'Anthropic API key from console.anthropic.com.',
    profiles: [
      {
        id: 'claude-cli',
        name: 'Claude CLI',
        description: 'Use the local Claude subscription session.',
        model: 'claude-haiku-4-5-20251001',
        baseUrl: 'https://api.anthropic.com',
        authMode: 'subscription'
      },
      {
        id: 'anthropic-api',
        name: 'Anthropic API',
        description: 'Use the hosted Anthropic API.',
        model: 'claude-haiku-4-5-20251001',
        baseUrl: 'https://api.anthropic.com',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'openai',
    name: 'OpenAI',
    eyebrow: 'Responses via compatibility',
    summary: 'OpenAI via Codex CLI subscription or direct API access.',
    defaultModel: 'gpt-5.4-mini',
    defaultBaseUrl: 'https://api.openai.com/v1',
    defaultAuth: 'subscription',
    authModes: ['subscription', 'api_key'],
    accent: 'border-emerald-300/30 bg-emerald-300/8 text-emerald-200',
    pill: 'text-emerald-200 border-emerald-300/30',
    subscriptionLabel: 'Codex CLI',
    subscriptionBinary: 'codex',
    subscriptionLoginCommand: 'codex login',
    keyPlaceholder: 'sk-proj-...',
    keyHint: 'OpenAI API key from platform.openai.com.',
    profiles: [
      {
        id: 'codex-cli',
        name: 'Codex CLI',
        description: 'Use the local Codex subscription session.',
        model: 'gpt-5.4-mini',
        baseUrl: 'https://api.openai.com/v1',
        authMode: 'subscription'
      },
      {
        id: 'openai-default',
        name: 'Hosted API',
        description: 'Use the standard OpenAI endpoint.',
        model: 'gpt-5.4-mini',
        baseUrl: 'https://api.openai.com/v1',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'gemini',
    name: 'Gemini',
    eyebrow: 'Google AI',
    summary: 'Gemini through the OpenAI-compatible endpoint.',
    defaultModel: 'gemini-2.5-flash',
    defaultBaseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
    defaultAuth: 'api_key',
    authModes: ['api_key'],
    accent: 'border-sky-300/30 bg-sky-300/8 text-sky-200',
    pill: 'text-sky-200 border-sky-300/30',
    keyPlaceholder: 'AIza...',
    keyHint: 'Gemini API key from Google AI Studio.',
    profiles: [
      {
        id: 'gemini-default',
        name: 'Gemini API',
        description: 'Use Google AI Studio.',
        model: 'gemini-2.5-flash',
        baseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'local',
    name: 'Local',
    eyebrow: 'Ollama / LM Studio',
    summary: 'Local OpenAI-compatible inference on the host.',
    defaultModel: 'gpt-oss',
    defaultBaseUrl: 'http://127.0.0.1:11434/v1',
    defaultAuth: 'none',
    authModes: ['none', 'api_key'],
    accent: 'border-stone-300/25 bg-stone-300/8 text-stone-200',
    pill: 'text-stone-200 border-stone-300/25',
    keyPlaceholder: 'optional',
    keyHint: 'Usually not required for local endpoints. LM Studio or reverse proxies may add one.',
    profiles: [
      {
        id: 'ollama',
        name: 'Ollama',
        description: 'Target Ollama on localhost.',
        model: 'gpt-oss',
        baseUrl: 'http://127.0.0.1:11434/v1',
        authMode: 'none'
      },
      {
        id: 'lm-studio',
        name: 'LM Studio',
        description: 'Use LM Studio\'s local server endpoint.',
        model: 'qwen3-8b',
        baseUrl: 'http://127.0.0.1:1234/v1',
        authMode: 'none'
      }
    ]
  }
]

const authModeLabels = {
  subscription: { name: 'Subscription', description: 'Use the local provider CLI session' },
  api_key: { name: 'API Key', description: 'Send a bearer key to the provider' },
  none: { name: 'No Auth', description: 'Call the endpoint without auth' }
}

const currentProvider = computed(() =>
  providerOptions.find(provider => provider.id === config.value.ai_provider) || providerOptions[0]
)

const liveProvider = computed(() =>
  providerOptions.find(provider => provider.id === aiStatus.value?.provider) || null
)

const currentSubscriptionLabel = computed(() => currentProvider.value.subscriptionLabel || 'Provider CLI')
const currentSubscriptionBinary = computed(() => currentProvider.value.subscriptionBinary || 'cli')
const currentSubscriptionLoginCommand = computed(() => currentProvider.value.subscriptionLoginCommand || '')
const liveSubscriptionLabel = computed(() => aiStatus.value?.subscription_cli || liveProvider.value?.subscriptionLabel || 'Provider CLI')

const availableAuthModes = computed(() => currentProvider.value.authModes)
const currentProfiles = computed(() => currentProvider.value.profiles || [])
const modelOptionsId = computed(() => `ai-model-options-${config.value.ai_provider || 'default'}`)
const hasStoredApiKey = computed(() => !!config.value.has_ai_api_key && !config.value.clear_ai_api_key)

const providerModelCatalog = computed(() => {
  if (!modelCatalog.value) return null
  if (modelCatalog.value.provider !== config.value.ai_provider) return null
  if (modelCatalog.value.base_url !== config.value.ai_base_url) return null
  if (modelCatalog.value.auth_mode !== config.value.ai_auth_mode) return null
  return modelCatalog.value
})

const canRefreshModels = computed(() => {
  if (!config.value.ai_base_url) return false
  if (config.value.ai_auth_mode === 'subscription') return false
  if (config.value.ai_auth_mode === 'none') return true
  return !!config.value.ai_api_key || hasStoredApiKey.value
})

const canTestDraft = computed(() => {
  if (!config.value.ai_model || !config.value.ai_base_url) return false
  if (config.value.ai_auth_mode === 'subscription') return true
  if (config.value.ai_auth_mode === 'none') return true
  return !!config.value.ai_api_key || hasStoredApiKey.value
})

const draftMatchesRuntime = computed(() => {
  if (!aiStatus.value) return false

  return aiStatus.value.enabled === (config.value.ai_enabled === 'enabled')
    && aiStatus.value.provider === config.value.ai_provider
    && aiStatus.value.model === config.value.ai_model
    && aiStatus.value.auth_mode === config.value.ai_auth_mode
    && aiStatus.value.base_url === config.value.ai_base_url
    && Number(aiStatus.value.timeout_ms || 0) === (Number(config.value.ai_timeout_ms) || 5000)
    && Number(aiStatus.value.cache_ttl_hours || 0) === (Number(config.value.ai_cache_ttl_hours) || 168)
})

function uniqueModelSuggestions(sources) {
  const merged = []
  const seen = new Set()

  sources.flat().forEach(model => {
    if (!model?.id || seen.has(model.id)) return
    seen.add(model.id)
    merged.push({
      id: model.id,
      label: model.label || model.id,
      origin: model.origin || 'preset'
    })
  })

  return merged
}

const discoveredModelSuggestions = computed(() =>
  ((providerModelCatalog.value?.models) || []).map(model => ({
    id: model.id,
    label: model.label || model.id,
    origin: 'live'
  }))
)

const fallbackModelSuggestions = computed(() => {
  const runtimeSuggestions = aiStatus.value?.provider === config.value.ai_provider && aiStatus.value?.model
    ? [{ id: aiStatus.value.model, label: 'Live runtime', origin: 'runtime' }]
    : []

  const profileSuggestions = currentProfiles.value
    .filter(profile => profile.model)
    .map(profile => ({
      id: profile.model,
      label: profile.name,
      origin: 'profile'
    }))

  const backendFallback = ((providerModelCatalog.value?.fallback_models) || []).map(model => ({
    id: model.id,
    label: model.label || model.id,
    origin: 'fallback'
  }))

  const currentSelection = config.value.ai_model
    ? [{ id: config.value.ai_model, label: 'Current selection', origin: 'current' }]
    : []

  return uniqueModelSuggestions([
    currentSelection,
    runtimeSuggestions,
    backendFallback,
    profileSuggestions,
    [{ id: currentProvider.value.defaultModel, label: 'Provider default', origin: 'default' }]
  ])
})

const modelSuggestions = computed(() =>
  uniqueModelSuggestions([discoveredModelSuggestions.value, fallbackModelSuggestions.value])
)

const featuredModelSuggestions = computed(() => modelSuggestions.value.slice(0, 8))

const modelDiscoverySummary = computed(() => {
  if (modelsLoading.value) {
    return { tone: 'text-ice', badge: 'Refreshing', text: 'Checking the provider for a live model catalog.' }
  }

  if (providerModelCatalog.value?.discovered) {
    const count = providerModelCatalog.value.model_count || providerModelCatalog.value.models?.length || 0
    return {
      tone: 'text-emerald-300',
      badge: 'Live list',
      text: `Discovered ${count} model${count === 1 ? '' : 's'} from ${currentProvider.value.name}.`
    }
  }

  if (providerModelCatalog.value?.error) {
    return {
      tone: config.value.ai_auth_mode === 'subscription' ? 'text-amber-200' : 'text-storm',
      badge: 'Preset hints',
      text: providerModelCatalog.value.error
    }
  }

  return {
    tone: 'text-storm',
    badge: 'Preset hints',
    text: 'Using provider defaults, profiles, and runtime hints for autocomplete suggestions.'
  }
})

const selectedHistoryEntry = computed(() => {
  if (!Array.isArray(aiHistory.value)) return null
  const device = (testDeviceName.value || '').trim()
  const app = (testAppName.value || '').trim()
  if (!device || !app) return null
  const exactKey = `${device}:${app}`
  return aiHistory.value.find(entry => entry.key === exactKey) || null
})

function optimizationSourceLabel(source) {
  switch (source) {
    case 'ai_live':
      return 'Live AI'
    case 'ai_cached':
      return 'Cached AI'
    case 'device_db':
      return 'Device tune'
    default:
      return source || 'Fallback'
  }
}

function confidenceTone(confidence) {
  switch ((confidence || '').toLowerCase()) {
    case 'high':
      return 'border-emerald-300/20 bg-emerald-300/8 text-emerald-200'
    case 'medium':
      return 'border-amber-300/20 bg-amber-300/8 text-amber-200'
    case 'low':
      return 'border-red-400/20 bg-red-400/8 text-red-300'
    default:
      return 'border-storm/40 bg-void/30 text-storm'
  }
}

function cacheStatusTone(status) {
  switch ((status || '').toLowerCase()) {
    case 'hit':
      return 'border-emerald-300/20 bg-emerald-300/8 text-emerald-200'
    case 'miss':
      return 'border-sky-300/20 bg-sky-300/8 text-sky-200'
    case 'invalidated':
      return 'border-red-400/20 bg-red-400/8 text-red-300'
    case 'stale':
      return 'border-amber-300/20 bg-amber-300/8 text-amber-200'
    default:
      return 'border-storm/40 bg-void/30 text-storm'
  }
}

function formatRelativeTime(timestamp) {
  if (!timestamp) return '—'
  const deltaSeconds = Math.max(0, Math.floor(Date.now() / 1000) - Number(timestamp))
  if (deltaSeconds < 60) return 'just now'
  if (deltaSeconds < 3600) return `${Math.floor(deltaSeconds / 60)}m ago`
  if (deltaSeconds < 86400) return `${Math.floor(deltaSeconds / 3600)}h ago`
  return `${Math.floor(deltaSeconds / 86400)}d ago`
}

const providerHealthSummary = computed(() => {
  if (!aiStatus.value) {
    return { tone: 'text-storm', label: 'Unknown', detail: 'Runtime state not loaded yet.' }
  }
  if (aiStatus.value.last_failure_at && (!aiStatus.value.last_success_at || aiStatus.value.last_failure_at >= aiStatus.value.last_success_at)) {
    return {
      tone: 'text-red-300',
      label: 'Attention',
      detail: aiStatus.value.last_error || 'The most recent provider request failed.'
    }
  }
  if (Number(aiStatus.value.in_flight_requests || 0) > 0) {
    return {
      tone: 'text-sky-200',
      label: 'Busy',
      detail: `${aiStatus.value.in_flight_requests} request${aiStatus.value.in_flight_requests === 1 ? '' : 's'} in flight.`
    }
  }
  return {
    tone: 'text-emerald-200',
    label: 'Healthy',
    detail: aiStatus.value.last_success_at ? `Last success ${formatRelativeTime(aiStatus.value.last_success_at)}.` : 'No provider calls have completed yet.'
  }
})

function providerPill(providerId) {
  const provider = providerOptions.find(item => item.id === providerId)
  return provider?.pill || 'text-silver border-storm/40'
}

function subscriptionRuntimeTone(status) {
  if (!status) return 'text-storm'
  if (status.cli_authenticated === true) return 'text-green-300'
  if (status.cli_authenticated === false) return 'text-amber-200'
  return status.cli_available ? 'text-green-300' : 'text-red-300'
}

function subscriptionRuntimeSummary(status) {
  if (!status) return 'Not loaded'
  if (status.cli_authenticated === true) return 'Signed in and ready'
  if (status.cli_authenticated === false && status.cli_login_command) return `Run ${status.cli_login_command}`
  return status.cli_available ? 'Detected in PATH' : 'Not found in PATH'
}

function applyProviderProfile(profile) {
  config.value.ai_model = profile.model || currentProvider.value.defaultModel
  config.value.ai_base_url = profile.baseUrl || currentProvider.value.defaultBaseUrl
  config.value.ai_auth_mode = profile.authMode || currentProvider.value.defaultAuth
  config.value.ai_use_subscription = config.value.ai_auth_mode === 'subscription' ? 'enabled' : 'disabled'

  if (config.value.ai_auth_mode === 'none') {
    config.value.ai_api_key = ''
  }

  testResult.value = null
}

function resetProviderDefaults() {
  applyProviderProfile({
    model: currentProvider.value.defaultModel,
    baseUrl: currentProvider.value.defaultBaseUrl,
    authMode: currentProvider.value.defaultAuth
  })
}

function isProfileActive(profile) {
  return config.value.ai_model === (profile.model || currentProvider.value.defaultModel)
    && config.value.ai_base_url === (profile.baseUrl || currentProvider.value.defaultBaseUrl)
    && config.value.ai_auth_mode === (profile.authMode || currentProvider.value.defaultAuth)
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

async function refreshModelCatalog({ silent = false } = {}) {
  const result = await fetchModels(buildDraftPayload())
  if (!silent && result?.discovered) {
    toast(`${currentProvider.value.name} model list refreshed`, 'success')
  }
}

function scheduleModelRefresh() {
  if (modelRefreshTimer) {
    clearTimeout(modelRefreshTimer)
  }

  modelRefreshTimer = setTimeout(() => {
    refreshModelCatalog({ silent: true })
  }, 350)
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

watch(() => config.value.ai_api_key, (nextKey, previousKey) => {
  if (!!nextKey !== !!previousKey) {
    scheduleModelRefresh()
  }
})

watch(
  () => [config.value.ai_provider, config.value.ai_auth_mode, config.value.ai_base_url],
  () => {
    scheduleModelRefresh()
  },
  { immediate: true }
)

function buildDraftPayload() {
  return {
    ai_provider: config.value.ai_provider,
    ai_model: config.value.ai_model,
    ai_auth_mode: config.value.ai_auth_mode,
    ai_api_key: config.value.ai_api_key || '',
    clear_ai_api_key: !!config.value.clear_ai_api_key,
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
  const result = await testConnection(
    buildDraftPayload(),
    testDeviceName.value || 'Steam Deck OLED',
    testAppName.value || 'Rocket League'
  )

  testLoading.value = false
  if (result.status) {
    const message = provider.id === 'local'
      ? `Local endpoint returned a valid optimization for ${testDeviceName.value || 'the selected device'}.`
      : `${provider.name} returned a valid optimization for ${testDeviceName.value || 'the selected device'}.`
    testResult.value = { success: true, message, detail: result.reasoning || '', payload: result }
    toast(`${provider.name} draft settings verified`, 'success')
  } else {
    testResult.value = {
      success: false,
      message: result.error || 'Connection test failed',
      detail: '',
      payload: null
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
  await Promise.all([fetchStatus(), fetchCache(), fetchHistory(), fetchDevices()])
  if (!testDeviceName.value) {
    const preferredDevice = aiDevices.value.find(device => /steam deck/i.test(device.name)) || aiDevices.value[0]
    testDeviceName.value = preferredDevice?.name || 'Test Device'
  }
  if (!testAppName.value) {
    testAppName.value = 'Rocket League'
  }
  filterDevices()
})

onBeforeUnmount(() => {
  if (modelRefreshTimer) {
    clearTimeout(modelRefreshTimer)
  }
})
</script>

<template>
  <div class="config-page space-y-6">
    <section class="settings-section space-y-4">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
        <div class="max-w-3xl">
          <div class="section-kicker">Provider</div>
          <h2 class="settings-section-title mt-2">Choose the optimizer.</h2>
          <p class="settings-section-copy">Pick the AI provider and adjust the live draft below before you save and apply it.</p>
        </div>

        <div class="flex items-center gap-3 rounded-2xl border border-storm/40 bg-void/30 px-4 py-3">
          <div>
            <div class="text-xs uppercase tracking-wider text-storm">AI runtime</div>
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
    </section>

    <div class="grid gap-6 xl:grid-cols-[1.4fr_0.9fr]">
      <div class="space-y-6">
        <section class="settings-section space-y-5">
          <div class="flex items-start justify-between gap-4">
            <div>
              <div class="section-kicker">Setup</div>
              <h3 class="settings-section-title mt-2">{{ currentProvider.name }} setup</h3>
            </div>
            <div class="flex items-center gap-2">
              <button
                @click="resetProviderDefaults"
                class="inline-flex items-center rounded-full border border-storm/40 px-2.5 py-1 text-[11px] font-medium text-storm transition-colors hover:border-ice/30 hover:text-silver">
                Reset defaults
              </button>
              <span class="inline-flex items-center rounded-full border px-2.5 py-1 text-[11px] font-medium" :class="providerPill(currentProvider.id)">
                {{ currentProvider.name }}
              </span>
            </div>
          </div>

          <div class="space-y-3">
            <div class="flex items-center justify-between gap-4">
              <div class="text-xs font-semibold uppercase tracking-[0.2em] text-storm">Profiles</div>
              <div class="text-[11px] text-storm">Model + endpoint + auth</div>
            </div>
            <div class="grid gap-2 sm:grid-cols-2">
              <button
                v-for="profile in currentProfiles"
                :key="profile.id"
                @click="applyProviderProfile(profile)"
                class="rounded-xl border px-4 py-3 text-left transition-all duration-200"
                :class="isProfileActive(profile) ? 'border-ice/40 bg-ice/10 text-ice' : 'border-storm/40 bg-void/30 text-silver hover:border-ice/30'">
                <div class="text-sm font-medium">{{ profile.name }}</div>
                <div class="text-xs text-storm mt-1">{{ profile.description }}</div>
                <div class="text-[11px] font-mono mt-2" :class="isProfileActive(profile) ? 'text-ice/90' : 'text-silver/70'">
                  {{ profile.model }}
                </div>
              </button>
            </div>
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
              <div class="flex items-center justify-between gap-3 mb-1">
                <label class="block text-sm font-medium text-silver">Model</label>
                <button
                  @click="refreshModelCatalog()"
                  :disabled="modelsLoading || !canRefreshModels"
                  class="inline-flex items-center gap-2 rounded-full border border-storm/40 px-2.5 py-1 text-[11px] font-medium text-storm transition-colors hover:border-ice/30 hover:text-silver disabled:cursor-not-allowed disabled:opacity-50">
                  <svg v-if="modelsLoading" class="h-3.5 w-3.5 animate-spin" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
                  <span>{{ modelsLoading ? 'Refreshing…' : 'Refresh list' }}</span>
                </button>
              </div>
              <input
                v-model="config.ai_model"
                :list="modelOptionsId"
                type="text"
                class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm"
                :placeholder="currentProvider.defaultModel" />
              <datalist :id="modelOptionsId">
                <option
                  v-for="model in modelSuggestions"
                  :key="model.id"
                  :value="model.id"
                  :label="model.label || model.id" />
              </datalist>
              <div class="mt-2 flex flex-wrap gap-2">
                <button
                  v-for="model in featuredModelSuggestions"
                  :key="model.id"
                  @click="config.ai_model = model.id"
                  class="inline-flex items-center rounded-full border px-2.5 py-1 text-[11px] font-medium transition-colors"
                  :class="config.ai_model === model.id ? 'border-ice/40 bg-ice/10 text-ice' : model.origin === 'live' ? 'border-emerald-300/20 bg-emerald-300/8 text-emerald-200 hover:border-emerald-300/35' : 'border-storm/40 bg-void/30 text-storm hover:border-ice/30 hover:text-silver'">
                  {{ model.id }}
                </button>
              </div>
              <div class="mt-2 flex items-start justify-between gap-3 text-xs">
                <div :class="modelDiscoverySummary.tone">{{ modelDiscoverySummary.text }}</div>
                <span class="inline-flex shrink-0 items-center rounded-full border px-2 py-0.5 text-[10px] font-medium uppercase tracking-[0.2em]" :class="providerModelCatalog?.discovered ? 'border-emerald-300/20 bg-emerald-300/8 text-emerald-200' : 'border-storm/40 bg-void/30 text-storm'">
                  {{ modelDiscoverySummary.badge }}
                </span>
              </div>
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
            <div class="text-xs uppercase tracking-[0.2em] text-storm">{{ currentSubscriptionLabel }}</div>
            <div class="text-sm text-silver">Polaris will call the local <code class="bg-void/40 px-1 rounded text-amber-200">{{ currentSubscriptionBinary }}</code> CLI instead of a remote API key flow.</div>
            <div class="text-xs text-storm mt-2">The Web UI can test the draft config below, but live sessions still switch over only after save and apply.</div>
            <div v-if="currentSubscriptionLoginCommand" class="text-xs text-storm mt-2">
              If this host is not authorized yet, run <code class="bg-void/40 px-1 rounded text-amber-200">{{ currentSubscriptionLoginCommand }}</code> in a terminal first.
            </div>
          </div>

          <div v-else-if="config.ai_auth_mode === 'none'" class="rounded-xl border border-stone-300/20 bg-stone-300/6 p-4">
            <div class="text-sm text-silver">Polaris will call the configured endpoint without an Authorization header.</div>
            <div class="text-xs text-storm mt-2">This is the usual setup for Ollama on <code class="bg-void/40 px-1 rounded">http://127.0.0.1:11434/v1</code>.</div>
          </div>

          <div v-else>
            <label class="block text-sm font-medium text-silver mb-1">API Key</label>
            <div v-if="hasStoredApiKey" class="mb-2 flex items-center justify-between gap-3 rounded-xl border border-emerald-300/20 bg-emerald-300/8 px-3 py-2 text-xs text-emerald-100">
              <span>An API key is already stored on the host. Leave this blank to keep it, or type a new key to replace it.</span>
              <button
                type="button"
                class="rounded-full border border-emerald-300/25 px-2.5 py-1 text-[11px] font-medium text-emerald-100 transition-colors hover:border-rose-300/40 hover:text-rose-200"
                @click="config.clear_ai_api_key = true; config.ai_api_key = ''">
                Clear Stored Key
              </button>
            </div>
            <div v-else-if="config.clear_ai_api_key" class="mb-2 flex items-center justify-between gap-3 rounded-xl border border-rose-300/20 bg-rose-300/8 px-3 py-2 text-xs text-rose-100">
              <span>The stored API key will be removed when you save.</span>
              <button
                type="button"
                class="rounded-full border border-rose-300/30 px-2.5 py-1 text-[11px] font-medium text-rose-100 transition-colors hover:border-ice/40 hover:text-ice"
                @click="config.clear_ai_api_key = false">
                Keep Existing Key
              </button>
            </div>
            <div class="flex gap-2">
              <div class="relative flex-1">
                <input
                  :type="showApiKey ? 'text' : 'password'"
                  v-model="config.ai_api_key"
                  @input="config.ai_api_key && (config.clear_ai_api_key = false)"
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
        </section>

        <section class="settings-section settings-section-compact space-y-4">
          <div class="flex items-center justify-between gap-3">
            <div>
              <div class="section-kicker">Testing</div>
              <div class="settings-section-title mt-2 text-base">Cache, timeout, and draft checks</div>
            </div>
            <button
              @click="testProviderConfig"
              :disabled="testLoading || aiLoading || !canTestDraft"
              class="inline-flex items-center gap-2 h-10 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 transition-all duration-200 disabled:opacity-50 disabled:cursor-not-allowed">
              <svg v-if="testLoading || aiLoading" class="w-4 h-4 animate-spin" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
              <span>{{ testLoading || aiLoading ? 'Testing…' : 'Test draft' }}</span>
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

          <div class="grid gap-4 lg:grid-cols-2">
            <div>
              <label class="block text-sm font-medium text-silver mb-1">Test Device</label>
              <input
                v-model="testDeviceName"
                list="ai-device-options"
                type="text"
                class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
                placeholder="Steam Deck OLED" />
              <div class="text-xs text-storm mt-1">Use a real or close-match device name.</div>
              <datalist id="ai-device-options">
                <option v-for="device in aiDevices" :key="device.name" :value="device.name" />
              </datalist>
            </div>

            <div>
              <label class="block text-sm font-medium text-silver mb-1">Test App</label>
              <input
                v-model="testAppName"
                type="text"
                class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
                placeholder="Rocket League" />
              <div class="text-xs text-storm mt-1">Optional title or genre cue.</div>
            </div>
          </div>

          <div v-if="testResult" class="rounded-xl border px-4 py-3" :class="testResult.success ? 'border-green-400/20 bg-green-400/8' : 'border-red-400/20 bg-red-400/8'">
            <div class="text-sm font-medium" :class="testResult.success ? 'text-green-300' : 'text-red-300'">{{ testResult.message }}</div>
            <div v-if="testResult.detail" class="text-xs text-silver/70 mt-2">{{ testResult.detail }}</div>
            <div v-if="testResult.success && testResult.payload" class="grid gap-2 mt-3 sm:grid-cols-2">
              <div class="rounded-lg border border-storm/20 bg-void/40 px-3 py-2">
                <div class="text-[10px] uppercase tracking-wider text-storm">Source</div>
                <div class="mt-2 flex flex-wrap items-center gap-2">
                  <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="cacheStatusTone(testResult.payload.cache_status)">
                    {{ optimizationSourceLabel(testResult.payload.source) }}
                  </span>
                  <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="confidenceTone(testResult.payload.confidence)">
                    {{ (testResult.payload.confidence || 'unknown').toUpperCase() }}
                  </span>
                </div>
              </div>
              <div class="rounded-lg border border-storm/20 bg-void/40 px-3 py-2">
                <div class="text-[10px] uppercase tracking-wider text-storm">Freshness</div>
                <div class="text-sm text-silver mt-1">
                  {{ formatRelativeTime(testResult.payload.generated_at) }}
                  <span class="text-storm"> · expires {{ formatRelativeTime(testResult.payload.expires_at) }}</span>
                </div>
              </div>
              <div v-if="testResult.payload.display_mode" class="rounded-lg border border-storm/20 bg-void/40 px-3 py-2">
                <div class="text-[10px] uppercase tracking-wider text-storm">Display</div>
                <div class="text-sm font-mono text-silver mt-1">{{ testResult.payload.display_mode }}</div>
              </div>
              <div v-if="testResult.payload.target_bitrate_kbps" class="rounded-lg border border-storm/20 bg-void/40 px-3 py-2">
                <div class="text-[10px] uppercase tracking-wider text-storm">Bitrate</div>
                <div class="text-sm font-mono text-silver mt-1">{{ (testResult.payload.target_bitrate_kbps / 1000).toFixed(1) }} Mbps</div>
              </div>
              <div v-if="testResult.payload.preferred_codec" class="rounded-lg border border-storm/20 bg-void/40 px-3 py-2">
                <div class="text-[10px] uppercase tracking-wider text-storm">Codec</div>
                <div class="text-sm font-mono text-silver mt-1">{{ testResult.payload.preferred_codec.toUpperCase() }}</div>
              </div>
              <div v-if="testResult.payload.hdr != null" class="rounded-lg border border-storm/20 bg-void/40 px-3 py-2">
                <div class="text-[10px] uppercase tracking-wider text-storm">HDR</div>
                <div class="text-sm font-mono mt-1" :class="testResult.payload.hdr ? 'text-green-300' : 'text-storm'">
                  {{ testResult.payload.hdr ? 'Enabled' : 'Disabled' }}
                </div>
              </div>
            </div>
            <div v-if="testResult.success && testResult.payload?.signals_used?.length" class="mt-3 rounded-lg border border-storm/20 bg-void/40 px-3 py-2">
              <div class="text-[10px] uppercase tracking-wider text-storm">Signals used</div>
              <div class="mt-2 flex flex-wrap gap-2">
                <span v-for="signal in testResult.payload.signals_used" :key="signal" class="inline-flex items-center rounded-full border border-storm/40 bg-void/30 px-2 py-0.5 text-[11px] font-medium text-silver">
                  {{ signal }}
                </span>
              </div>
            </div>
            <div v-if="testResult.success && testResult.payload?.normalization_reason" class="mt-3 rounded-lg border border-amber-300/20 bg-amber-300/6 px-3 py-2">
              <div class="text-[10px] uppercase tracking-wider text-storm">Normalization</div>
              <div class="text-xs text-silver mt-2">{{ testResult.payload.normalization_reason }}</div>
            </div>
            <div v-if="selectedHistoryEntry" class="mt-3 rounded-lg border px-3 py-2" :class="selectedHistoryEntry.consecutive_poor_outcomes > 0 ? 'border-red-400/20 bg-red-400/8' : 'border-storm/20 bg-void/40'">
              <div class="text-[10px] uppercase tracking-wider text-storm">Recent outcome for this device + app</div>
              <div class="mt-2 flex flex-wrap items-center gap-2">
                <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="confidenceTone(selectedHistoryEntry.last_optimization_confidence)">
                  {{ optimizationSourceLabel(selectedHistoryEntry.last_optimization_source) }}
                </span>
                <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="(selectedHistoryEntry.last_quality_grade || selectedHistoryEntry.quality_grade) === 'A' || (selectedHistoryEntry.last_quality_grade || selectedHistoryEntry.quality_grade) === 'B' ? 'border-emerald-300/20 bg-emerald-300/8 text-emerald-200' : (selectedHistoryEntry.last_quality_grade || selectedHistoryEntry.quality_grade) === 'C' ? 'border-amber-300/20 bg-amber-300/8 text-amber-200' : 'border-red-400/20 bg-red-400/8 text-red-300'">
                  Grade {{ selectedHistoryEntry.last_quality_grade || selectedHistoryEntry.quality_grade || '—' }}
                </span>
                <span class="text-xs text-storm">Updated {{ formatRelativeTime(selectedHistoryEntry.last_updated_at) }}</span>
              </div>
              <div class="text-xs text-silver mt-2">
                Latest session: {{ Math.round(selectedHistoryEntry.last_fps || selectedHistoryEntry.avg_fps || 0) }}/{{ Math.round(selectedHistoryEntry.last_target_fps || selectedHistoryEntry.last_fps || selectedHistoryEntry.avg_fps || 0) }} FPS,
                {{ Math.round(selectedHistoryEntry.last_latency_ms || selectedHistoryEntry.avg_latency_ms || 0) }}ms,
                {{ selectedHistoryEntry.last_bitrate_kbps || selectedHistoryEntry.avg_bitrate_kbps || 0 }}kbps,
                {{ Number(selectedHistoryEntry.last_packet_loss_pct ?? selectedHistoryEntry.packet_loss_pct ?? 0).toFixed(1) }}% loss.
              </div>
              <div class="text-xs text-silver mt-2">
                {{ selectedHistoryEntry.session_count }} session{{ selectedHistoryEntry.session_count === 1 ? '' : 's' }} tracked,
                avg {{ Math.round(selectedHistoryEntry.avg_fps || 0) }} FPS,
                {{ selectedHistoryEntry.poor_outcome_count }} poor outcome{{ selectedHistoryEntry.poor_outcome_count === 1 ? '' : 's' }} total,
                {{ selectedHistoryEntry.consecutive_poor_outcomes }} consecutive poor sessions.
              </div>
              <div v-if="selectedHistoryEntry.last_invalidated_at" class="text-xs text-amber-200 mt-2">
                Cache invalidated {{ formatRelativeTime(selectedHistoryEntry.last_invalidated_at) }} after the last poor run.
              </div>
            </div>
          </div>
        </section>
      </div>

      <div class="space-y-6">
        <details class="settings-section settings-section-compact settings-disclosure" :open="false">
          <summary class="settings-disclosure-summary">
            <div>
              <div class="section-kicker">Runtime</div>
              <div class="settings-section-title mt-2 text-base">Saved optimizer status</div>
              <div class="settings-summary-copy">The loaded runtime can differ from the unsaved draft on the left.</div>
            </div>
            <div class="flex items-center gap-2">
              <span
                class="inline-flex items-center rounded-full border px-2.5 py-1 text-[11px] font-medium"
                :class="draftMatchesRuntime ? 'border-green-400/20 bg-green-400/8 text-green-300' : 'border-amber-300/20 bg-amber-300/8 text-amber-200'">
                {{ draftMatchesRuntime ? 'In sync' : 'Unsaved draft' }}
              </span>
              <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
            </div>
          </summary>

          <div class="settings-disclosure-body grid gap-3">
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

            <div class="grid gap-3 sm:grid-cols-2">
              <div class="rounded-xl border border-storm/30 bg-void/30 p-3">
                <div class="text-xs uppercase tracking-wider text-storm">Provider Health</div>
                <div class="text-sm font-medium mt-2" :class="providerHealthSummary.tone">{{ providerHealthSummary.label }}</div>
                <div class="text-xs text-storm mt-2">{{ providerHealthSummary.detail }}</div>
              </div>

              <div class="rounded-xl border border-storm/30 bg-void/30 p-3">
                <div class="text-xs uppercase tracking-wider text-storm">Runtime Telemetry</div>
                <div class="mt-2 grid gap-2 text-xs text-silver">
                  <div class="flex items-center justify-between gap-3">
                    <span class="text-storm">Last latency</span>
                    <span class="font-mono">{{ aiStatus?.last_latency_ms ?? '—' }}<span v-if="aiStatus?.last_latency_ms != null"> ms</span></span>
                  </div>
                  <div class="flex items-center justify-between gap-3">
                    <span class="text-storm">Last success</span>
                    <span>{{ formatRelativeTime(aiStatus?.last_success_at) }}</span>
                  </div>
                  <div class="flex items-center justify-between gap-3">
                    <span class="text-storm">Last failure</span>
                    <span>{{ formatRelativeTime(aiStatus?.last_failure_at) }}</span>
                  </div>
                  <div class="flex items-center justify-between gap-3">
                    <span class="text-storm">In flight</span>
                    <span class="font-mono">{{ aiStatus?.in_flight_requests ?? 0 }}</span>
                  </div>
                </div>
              </div>
            </div>

            <div v-if="aiStatus?.auth_mode === 'subscription'" class="rounded-xl border border-amber-300/20 bg-amber-300/6 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">{{ liveSubscriptionLabel }}</div>
              <div class="text-sm mt-2" :class="subscriptionRuntimeTone(aiStatus)">
                {{ subscriptionRuntimeSummary(aiStatus) }}
              </div>
              <div v-if="aiStatus?.cli_authenticated === false && aiStatus?.cli_login_command" class="text-xs text-storm mt-2">
                Authorize this host by running <code class="bg-void/40 px-1 rounded text-amber-200">{{ aiStatus.cli_login_command }}</code> in a terminal.
              </div>
            </div>

            <div v-if="aiStatus && !draftMatchesRuntime" class="rounded-xl border border-amber-300/20 bg-amber-300/6 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">Pending Change</div>
              <div class="text-sm text-silver mt-2">The draft on the left differs from the loaded runtime. Save and apply before expecting live sessions to switch providers or models.</div>
            </div>

            <div v-if="aiStatus?.last_error" class="rounded-xl border border-red-400/20 bg-red-400/8 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">Recent Error</div>
              <div class="text-sm text-red-300 mt-2">{{ aiStatus.last_error }}</div>
            </div>
          </div>
        </details>

        <details class="settings-section settings-section-compact settings-disclosure" :open="cacheExpanded" @toggle="cacheExpanded = $event.target.open">
          <summary class="settings-disclosure-summary">
            <div class="flex items-center gap-2">
              <div class="section-kicker">Cache</div>
              <span class="px-1.5 py-0.5 rounded text-xs font-mono bg-twilight text-silver">{{ Array.isArray(aiCache) ? aiCache.length : 0 }}</span>
            </div>
            <div class="flex items-center gap-2">
              <button
                @click.stop="handleClearCache"
                class="text-xs text-storm hover:text-red-400 transition-colors"
                :class="{ 'opacity-50 pointer-events-none': !Array.isArray(aiCache) || aiCache.length === 0 }">
                Clear All
              </button>
              <svg class="settings-disclosure-chevron h-4 w-4 text-storm" :class="{ 'rotate-180': cacheExpanded }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
            </div>
          </summary>
          <div class="settings-disclosure-body text-sm text-storm">Stored recommendations for known device and app pairs.</div>

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
                <div class="flex flex-wrap items-center gap-2 pb-1.5 border-b border-storm/20">
                  <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="cacheStatusTone(entry.cache_status)">
                    {{ entry.cache_status || 'stored' }}
                  </span>
                  <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="confidenceTone(entry.confidence)">
                    {{ (entry.confidence || 'unknown').toUpperCase() }}
                  </span>
                  <span class="text-storm">updated {{ formatRelativeTime(entry.generated_at || entry.cached_at) }}</span>
                </div>
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
                <div class="flex justify-between" v-if="entry.expires_at">
                  <span class="text-storm">Expires</span>
                  <span class="text-silver">{{ formatRelativeTime(entry.expires_at) }}</span>
                </div>
                <div v-if="entry.signals_used?.length" class="pt-1.5 border-t border-storm/20">
                  <div class="text-storm mb-2">Signals used</div>
                  <div class="flex flex-wrap gap-1.5">
                    <span v-for="signal in entry.signals_used" :key="signal" class="inline-flex items-center rounded-full border border-storm/40 bg-void/30 px-2 py-0.5 text-[11px] font-medium text-silver">
                      {{ signal }}
                    </span>
                  </div>
                </div>
                <div v-if="entry.normalization_reason" class="pt-1.5 border-t border-storm/20">
                  <span class="text-storm">Normalization: </span>
                  <span class="text-silver/80">{{ entry.normalization_reason }}</span>
                </div>
                <div v-if="entry.reasoning" class="pt-1.5 border-t border-storm/20">
                  <span class="text-storm">AI Reasoning: </span>
                  <span class="text-silver/80 italic">{{ entry.reasoning }}</span>
                </div>
              </div>
            </div>
          </div>
          <div v-else-if="cacheExpanded" class="text-sm text-storm text-center py-3">No cached optimizations yet</div>
        </details>
      </div>
    </div>

    <details class="settings-section settings-section-compact settings-disclosure" :open="knowledgeExpanded" @toggle="knowledgeExpanded = $event.target.open">
      <summary class="settings-disclosure-summary">
        <div class="flex items-center gap-2">
          <div class="section-kicker">Devices</div>
          <span class="px-1.5 py-0.5 rounded text-xs font-mono bg-twilight text-silver">{{ filteredDevices.length }} devices</span>
        </div>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" :class="{ 'rotate-180': knowledgeExpanded }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
      </summary>
      <div class="settings-disclosure-body text-sm text-storm">Known device hints used to seed recommendations.</div>
      <div v-if="knowledgeExpanded" class="mt-4">
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
    </details>
  </div>
</template>
