<script setup>
import { computed, inject, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useAiOptimizer } from '../../composables/useAiOptimizer'
import { useToast } from '../../composables/useToast'
import SelectableCard from '../../components/SelectableCard.vue'
import StatTile from '../../components/StatTile.vue'
import { aiReadinessCopy, describeAiReadiness } from '../../doctor-ai-readiness.js'

const props = defineProps(['config'])
const config = ref(props.config)
const $t = inject('i18n').t

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
    eyebrowKey: 'config.ai_provider_anthropic_eyebrow',
    summaryKey: 'config.ai_provider_anthropic_summary',
    defaultModel: 'claude-haiku-4-5-20251001',
    defaultBaseUrl: 'https://api.anthropic.com',
    defaultAuth: 'subscription',
    authModes: ['subscription', 'api_key'],
    accent: 'border-warning/30 bg-warning/8 text-warning-bright',
    pill: 'text-warning-bright border-warning/30',
    subscriptionLabel: 'Claude CLI',
    subscriptionBinary: 'claude',
    keyPlaceholder: 'sk-ant-api03-...',
    keyHintKey: 'config.ai_provider_anthropic_key_hint',
    profiles: [
      {
        id: 'claude-cli',
        name: 'Claude CLI',
        descriptionKey: 'config.ai_profile_claude_cli_desc',
        model: 'claude-haiku-4-5-20251001',
        baseUrl: 'https://api.anthropic.com',
        authMode: 'subscription'
      },
      {
        id: 'anthropic-api',
        name: 'Anthropic API',
        descriptionKey: 'config.ai_profile_anthropic_api_desc',
        model: 'claude-haiku-4-5-20251001',
        baseUrl: 'https://api.anthropic.com',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'openai',
    name: 'OpenAI',
    eyebrowKey: 'config.ai_provider_openai_eyebrow',
    summaryKey: 'config.ai_provider_openai_summary',
    defaultModel: 'gpt-5.4-mini',
    defaultBaseUrl: 'https://api.openai.com/v1',
    defaultAuth: 'subscription',
    authModes: ['subscription', 'api_key'],
    accent: 'border-success/30 bg-success/8 text-success-bright',
    pill: 'text-success-bright border-success/30',
    subscriptionLabel: 'Codex CLI',
    subscriptionBinary: 'codex',
    subscriptionLoginCommand: 'codex login',
    keyPlaceholder: 'sk-proj-...',
    keyHintKey: 'config.ai_provider_openai_key_hint',
    profiles: [
      {
        id: 'codex-cli',
        name: 'Codex CLI',
        descriptionKey: 'config.ai_profile_codex_cli_desc',
        model: 'gpt-5.4-mini',
        baseUrl: 'https://api.openai.com/v1',
        authMode: 'subscription'
      },
      {
        id: 'openai-default',
        name: 'Hosted API',
        descriptionKey: 'config.ai_profile_openai_default_desc',
        model: 'gpt-5.4-mini',
        baseUrl: 'https://api.openai.com/v1',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'gemini',
    name: 'Gemini',
    eyebrowKey: 'config.ai_provider_gemini_eyebrow',
    summaryKey: 'config.ai_provider_gemini_summary',
    defaultModel: 'gemini-2.5-flash',
    defaultBaseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
    defaultAuth: 'api_key',
    authModes: ['api_key'],
    accent: 'border-info/30 bg-info/8 text-info-bright',
    pill: 'text-info-bright border-info/30',
    keyPlaceholder: 'AIza...',
    keyHintKey: 'config.ai_provider_gemini_key_hint',
    profiles: [
      {
        id: 'gemini-default',
        name: 'Gemini API',
        descriptionKey: 'config.ai_profile_gemini_default_desc',
        model: 'gemini-2.5-flash',
        baseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'local',
    name: 'Local',
    eyebrowKey: 'config.ai_provider_local_eyebrow',
    summaryKey: 'config.ai_provider_local_summary',
    defaultModel: 'gpt-oss',
    defaultBaseUrl: 'http://127.0.0.1:11434/v1',
    defaultAuth: 'none',
    authModes: ['none', 'api_key'],
    accent: 'border-storm/25 bg-storm/8 text-silver',
    pill: 'text-silver border-storm/25',
    keyPlaceholder: 'optional',
    keyHintKey: 'config.ai_provider_local_key_hint',
    profiles: [
      {
        id: 'ollama',
        name: 'Ollama',
        descriptionKey: 'config.ai_profile_ollama_desc',
        model: 'gpt-oss',
        baseUrl: 'http://127.0.0.1:11434/v1',
        authMode: 'none'
      },
      {
        id: 'lm-studio',
        name: 'LM Studio',
        descriptionKey: 'config.ai_profile_lm_studio_desc',
        model: 'qwen3-8b',
        baseUrl: 'http://127.0.0.1:1234/v1',
        authMode: 'none'
      }
    ]
  }
]

const authModeLabels = {
  subscription: { nameKey: 'config.ai_auth_subscription', descriptionKey: 'config.ai_auth_subscription_desc' },
  api_key: { nameKey: 'config.ai_auth_api_key', descriptionKey: 'config.ai_auth_api_key_desc' },
  none: { nameKey: 'config.ai_auth_none', descriptionKey: 'config.ai_auth_none_desc' }
}

function authModeName(mode) {
  return authModeLabels[mode] ? $t(authModeLabels[mode].nameKey) : mode
}

// The saved runtime's readiness, read the same way Doctor & Support reads it.
const aiReadiness = computed(() => describeAiReadiness(aiStatus.value, config.value))
const aiReadinessText = computed(() => aiReadinessCopy(aiReadiness.value, $t))

const currentProvider = computed(() =>
  providerOptions.find(provider => provider.id === config.value.ai_provider) || providerOptions[0]
)

const liveProvider = computed(() =>
  providerOptions.find(provider => provider.id === aiStatus.value?.provider) || null
)

const currentSubscriptionLabel = computed(() => currentProvider.value.subscriptionLabel || $t('config.ai_provider_cli_label'))
const currentSubscriptionBinary = computed(() => currentProvider.value.subscriptionBinary || 'cli')
const currentSubscriptionLoginCommand = computed(() => currentProvider.value.subscriptionLoginCommand || '')
const liveSubscriptionLabel = computed(() => aiStatus.value?.subscription_cli || liveProvider.value?.subscriptionLabel || $t('config.ai_provider_cli_label'))

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

const aiExplanationsEnabled = computed(() => config.value.ai_enabled === 'enabled')

const authReady = computed(() => {
  if (config.value.ai_auth_mode === 'none') return true
  if (config.value.ai_auth_mode === 'api_key') return !!config.value.ai_api_key || hasStoredApiKey.value
  if (!aiStatus.value || aiStatus.value.provider !== config.value.ai_provider) return false
  if (aiStatus.value.cli_authenticated === true) return true
  return aiStatus.value.cli_available === true && aiStatus.value.cli_authenticated == null
})

const authHelpText = computed(() => {
  if (config.value.ai_auth_mode === 'api_key') return hasStoredApiKey.value ? $t('config.ai_auth_stored_key_ready') : $t('config.ai_auth_add_key')
  if (config.value.ai_auth_mode === 'none') return $t('config.ai_auth_none_needed')
  if (aiStatus.value?.provider === config.value.ai_provider && aiStatus.value?.cli_login_command) {
    return $t('config.ai_auth_run_command', { command: aiStatus.value.cli_login_command })
  }
  if (currentSubscriptionLoginCommand.value) return $t('config.ai_auth_run_command', { command: currentSubscriptionLoginCommand.value })
  return $t('config.ai_auth_sign_in_with', { label: currentSubscriptionLabel.value })
})

const setupSteps = computed(() => [
  {
    label: $t('config.ai_step_provider'),
    status: config.value.ai_provider ? currentProvider.value.name : $t('config.ai_step_needed'),
    done: !!config.value.ai_provider
  },
  {
    label: $t('config.ai_step_auth'),
    status: authReady.value ? $t('config.ai_step_ready') : authHelpText.value,
    done: authReady.value
  },
  {
    label: $t('config.ai_step_test'),
    status: testResult.value?.success ? $t('config.ai_step_passed') : $t('config.ai_step_run_before_saving'),
    done: testResult.value?.success === true
  },
  {
    label: $t('config.ai_step_enable'),
    status: aiExplanationsEnabled.value ? $t('config.ai_state_enabled') : $t('config.ai_step_turn_on'),
    done: aiExplanationsEnabled.value
  }
])

function setAiExplanationsEnabled(enabled) {
  config.value.ai_enabled = enabled ? 'enabled' : 'disabled'
}

const draftMatchesRuntime = computed(() => {
  if (!aiStatus.value) return false

  return aiStatus.value.enabled === aiExplanationsEnabled.value
    && aiStatus.value.provider === config.value.ai_provider
    && aiStatus.value.model === config.value.ai_model
    && aiStatus.value.auth_mode === config.value.ai_auth_mode
    && aiStatus.value.base_url === config.value.ai_base_url
    && (aiStatus.value.codex_home || '') === (config.value.ai_codex_home || '')
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
    ? [{ id: aiStatus.value.model, label: $t('config.ai_model_origin_runtime'), origin: 'runtime' }]
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
    ? [{ id: config.value.ai_model, label: $t('config.ai_model_origin_current'), origin: 'current' }]
    : []

  return uniqueModelSuggestions([
    currentSelection,
    runtimeSuggestions,
    backendFallback,
    profileSuggestions,
    [{ id: currentProvider.value.defaultModel, label: $t('config.ai_model_origin_default'), origin: 'default' }]
  ])
})

const modelSuggestions = computed(() =>
  uniqueModelSuggestions([discoveredModelSuggestions.value, fallbackModelSuggestions.value])
)

const featuredModelSuggestions = computed(() => modelSuggestions.value.slice(0, 8))

const modelDiscoverySummary = computed(() => {
  if (modelsLoading.value) {
    return { tone: 'text-ice', badge: $t('config.ai_discovery_refreshing_badge'), text: $t('config.ai_discovery_refreshing_text') }
  }

  if (providerModelCatalog.value?.discovered) {
    const count = providerModelCatalog.value.model_count || providerModelCatalog.value.models?.length || 0
    return {
      tone: 'text-success',
      badge: $t('config.ai_discovery_live_badge'),
      text: $t('config.ai_discovery_live_text', { count, provider: currentProvider.value.name })
    }
  }

  if (providerModelCatalog.value?.error) {
    return {
      tone: config.value.ai_auth_mode === 'subscription' ? 'text-warning-bright' : 'text-storm',
      badge: $t('config.ai_discovery_preset_badge'),
      text: providerModelCatalog.value.error
    }
  }

  return {
    tone: 'text-storm',
    badge: $t('config.ai_discovery_preset_badge'),
    text: $t('config.ai_discovery_preset_text')
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
    case 'ai_explanation_test':
      return $t('config.ai_source_explanation_test')
    case 'ai_live':
      return $t('config.ai_source_live')
    case 'ai_cached':
      return $t('config.ai_source_cached')
    case 'device_db':
      return $t('config.ai_source_device_db')
    default:
      return source || $t('config.ai_source_fallback')
  }
}

function confidenceTone(confidence) {
  switch ((confidence || '').toLowerCase()) {
    case 'high':
      return 'border-success/20 bg-success/8 text-success-bright'
    case 'medium':
      return 'border-warning/20 bg-warning/8 text-warning-bright'
    case 'low':
      return 'border-danger/20 bg-danger/8 text-danger'
    default:
      return 'border-storm/40 bg-void/30 text-storm'
  }
}

function cacheStatusTone(status) {
  switch ((status || '').toLowerCase()) {
    case 'hit':
      return 'border-success/20 bg-success/8 text-success-bright'
    case 'miss':
      return 'border-info/20 bg-info/8 text-info-bright'
    case 'invalidated':
      return 'border-danger/20 bg-danger/8 text-danger'
    case 'stale':
      return 'border-warning/20 bg-warning/8 text-warning-bright'
    default:
      return 'border-storm/40 bg-void/30 text-storm'
  }
}

function formatRelativeTime(timestamp) {
  if (!timestamp) return $t('config.ai_time_never')
  const deltaSeconds = Math.max(0, Math.floor(Date.now() / 1000) - Number(timestamp))
  if (deltaSeconds < 60) return $t('config.ai_time_just_now')
  if (deltaSeconds < 3600) return $t('config.ai_time_minutes_ago', { count: Math.floor(deltaSeconds / 60) })
  if (deltaSeconds < 86400) return $t('config.ai_time_hours_ago', { count: Math.floor(deltaSeconds / 3600) })
  return $t('config.ai_time_days_ago', { count: Math.floor(deltaSeconds / 86400) })
}

const providerHealthSummary = computed(() => {
  if (!aiStatus.value) {
    return { tone: 'text-storm', label: $t('config.ai_unknown'), detail: $t('config.ai_health_unknown_detail') }
  }
  if (aiStatus.value.last_failure_at && (!aiStatus.value.last_success_at || aiStatus.value.last_failure_at >= aiStatus.value.last_success_at)) {
    return {
      tone: 'text-danger',
      label: $t('config.ai_health_attention'),
      detail: aiStatus.value.last_error || $t('config.ai_health_attention_detail')
    }
  }
  if (Number(aiStatus.value.in_flight_requests || 0) > 0) {
    return {
      tone: 'text-info-bright',
      label: $t('config.ai_health_busy'),
      detail: $t('config.ai_health_busy_detail', { count: aiStatus.value.in_flight_requests })
    }
  }
  return {
    tone: 'text-success-bright',
    label: $t('config.ai_health_healthy'),
    detail: aiStatus.value.last_success_at ? $t('config.ai_health_last_success', { when: formatRelativeTime(aiStatus.value.last_success_at) }) : $t('config.ai_health_no_calls')
  }
})

function providerPill(providerId) {
  const provider = providerOptions.find(item => item.id === providerId)
  return provider?.pill || 'text-silver border-storm/40'
}

function providerAuthSummary(provider) {
  return provider.authModes.map(authModeName).join(', ')
}

function providerRuntimeSummary(provider) {
  if (!aiStatus.value || aiStatus.value.provider !== provider.id) return $t('config.ai_runtime_not_saved')
  if (aiStatus.value.enabled) return $t('config.ai_runtime_saved_on')
  return $t('config.ai_runtime_saved_off')
}

function providerRuntimeTone(provider) {
  if (!aiStatus.value || aiStatus.value.provider !== provider.id) return 'text-storm'
  return aiStatus.value.enabled ? 'text-success-bright' : 'text-warning-bright'
}

function subscriptionRuntimeTone(status) {
  if (!status) return 'text-storm'
  if (status.cli_authenticated === true) return 'text-success'
  if (status.cli_authenticated === false) return 'text-warning-bright'
  return status.cli_available ? 'text-success' : 'text-danger'
}

function subscriptionRuntimeSummary(status) {
  if (!status) return $t('config.ai_not_loaded')
  if (status.cli_authenticated === true) return $t('config.ai_cli_signed_in')
  if (status.cli_authenticated === false && status.cli_login_command) return $t('config.ai_auth_run_command', { command: status.cli_login_command })
  return status.cli_available ? $t('config.ai_cli_detected') : $t('config.ai_cli_missing')
}

function applyProviderProfile(profile) {
  config.value.ai_model = profile.model || currentProvider.value.defaultModel
  config.value.ai_base_url = profile.baseUrl || currentProvider.value.defaultBaseUrl
  config.value.ai_auth_mode = profile.authMode || currentProvider.value.defaultAuth
  config.value.ai_timeout_ms = profile.timeoutMs || (currentProvider.value.id === 'local' ? 60000 : 5000)
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

  const previousDefaultTimeout = previousProvider?.id === 'local' ? 60000 : 5000
  const configuredTimeout = Number(config.value.ai_timeout_ms)
  const legacyLocalDefault = provider.id === 'local' && configuredTimeout === 5000
  if (!configuredTimeout || legacyLocalDefault || (previousProvider && configuredTimeout === previousDefaultTimeout)) {
    config.value.ai_timeout_ms = provider.id === 'local' ? 60000 : 5000
  }

  config.value.ai_use_subscription = config.value.ai_auth_mode === 'subscription' ? 'enabled' : 'disabled'

  if (config.value.ai_auth_mode === 'none') {
    config.value.ai_api_key = ''
  }
}

async function refreshModelCatalog({ silent = false } = {}) {
  const result = await fetchModels(buildDraftPayload())
  if (!silent && result?.discovered) {
    toast($t('config.ai_models_refreshed', { provider: currentProvider.value.name }), 'success')
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
    ai_codex_home: config.value.ai_codex_home || '',
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
    const device = testDeviceName.value || $t('config.ai_test_selected_device')
    const message = provider.id === 'local'
      ? $t('config.ai_test_local_returned', { device })
      : $t('config.ai_test_provider_returned', { provider: provider.name, device })
    testResult.value = {
      success: true,
      label: $t('config.ai_test_verified'),
      message,
      detail: result.reasoning || '',
      action: $t('config.ai_test_next_step'),
      payload: result
    }
    toast($t('config.ai_test_verified_toast', { provider: provider.name }), 'success')
  } else {
    const authFailure = result.code === 'authentication_failed' || /auth|authorized|login/i.test(result.error || '')
    testResult.value = {
      success: false,
      label: $t('config.ai_test_action_needed'),
      message: result.error || $t('config.ai_test_failed'),
      detail: result.detail || '',
      action: result.action || authHelpText.value,
      retryLabel: result.retryable === false ? $t('config.ai_test_review_settings') : (authFailure ? $t('config.ai_test_retry_auth') : $t('config.ai_test_retry')),
      payload: null
    }
    toast($t('config.ai_test_failed_toast', { provider: provider.name, error: result.error || $t('config.ai_unknown_error') }), 'error')
  }
}

async function handleClearCache() {
  const ok = await clearCache()
  if (ok) {
    toast($t('config.ai_cache_cleared'), 'success')
    fetchCache()
  } else {
    toast($t('config.ai_cache_clear_failed'), 'error')
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
          <div class="section-kicker">{{ $t('config.ai_tab_kicker') }}</div>
          <h2 class="settings-section-title mt-2">{{ $t('config.ai_tab_title') }}</h2>
          <p class="settings-section-copy">
            {{ $t('config.ai_tab_copy') }}
            <a href="https://papi-ux.com/docs/configuration/#ai-provider-settings" target="_blank" rel="noopener" class="focus-ring text-ice hover:underline">{{ $t('config.ai_tab_docs_link') }}</a>
          </p>
        </div>

        <div class="flex flex-col gap-3 rounded-2xl border border-storm/40 bg-void/30 px-4 py-3" data-ai-readiness-panel :data-ai-readiness="aiReadiness.state">
          <div class="flex items-center gap-3">
            <div>
              <div class="text-xs uppercase tracking-wider text-storm">{{ $t('config.ai_explanations') }}</div>
              <div class="text-sm font-medium text-silver mt-1">
                {{ aiExplanationsEnabled ? $t('config.ai_state_enabled') : $t('config.ai_state_disabled') }}
              </div>
            </div>
            <button
              type="button"
              role="switch"
              :aria-checked="aiExplanationsEnabled"
              :aria-label="$t('config.ai_toggle_aria')"
              @click="setAiExplanationsEnabled(!aiExplanationsEnabled)"
              class="focus-ring relative inline-flex h-6 w-11 shrink-0 cursor-pointer rounded-full transition-colors duration-200"
              :class="aiExplanationsEnabled ? 'bg-ice' : 'bg-storm/50'">
            <span
              class="inline-block h-5 w-5 transform rounded-full bg-white shadow-lg transition-transform duration-200 mt-0.5"
              :class="aiExplanationsEnabled ? 'translate-x-[22px]' : 'translate-x-0.5'"></span>
            </button>
          </div>
          <p class="max-w-xs text-xs leading-relaxed text-storm"><span class="text-silver/80">{{ $t('config.ai_saved_runtime_prefix') }}</span> {{ aiReadinessText }}</p>
        </div>
      </div>

      <div class="grid gap-3 lg:grid-cols-4">
        <SelectableCard
          v-for="provider in providerOptions"
          :key="provider.id"
          :selected="config.ai_provider === provider.id"
          :card-class="['rounded-2xl border p-4', config.ai_provider === provider.id ? provider.accent : 'border-storm/40 bg-deep hover:border-ice/40 hover:bg-twilight/30']"
          @click="config.ai_provider = provider.id">
          <div class="flex items-center justify-between gap-3">
            <div>
              <div class="text-[10px] uppercase tracking-eyebrow" :class="config.ai_provider === provider.id ? 'text-current/80' : 'text-storm'">{{ $t(provider.eyebrowKey) }}</div>
              <div class="text-base font-semibold mt-1" :class="config.ai_provider === provider.id ? 'text-current' : 'text-silver'">{{ provider.name }}</div>
            </div>
            <span
              class="h-2.5 w-2.5 rounded-full"
              :class="config.ai_provider === provider.id ? 'bg-current' : 'bg-storm/50'"></span>
          </div>
          <p class="text-sm mt-3 leading-6" :class="config.ai_provider === provider.id ? 'text-current/90' : 'text-storm'">
            {{ $t(provider.summaryKey) }}
          </p>
          <div class="mt-3 space-y-1 text-[11px] leading-5" :class="config.ai_provider === provider.id ? 'text-current/85' : 'text-storm'">
            <div>{{ $t('config.ai_auth_summary', { modes: providerAuthSummary(provider) }) }}</div>
            <div :class="providerRuntimeTone(provider)">{{ providerRuntimeSummary(provider) }}</div>
          </div>
        </SelectableCard>
      </div>

      <div class="rounded-2xl border border-ice/15 bg-ice/5 p-4">
        <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
          <div>
            <div class="section-kicker">{{ $t('config.ai_recommended_kicker') }}</div>
            <div class="settings-section-title mt-2 text-base">{{ $t('config.ai_recommended_title') }}</div>
            <p class="settings-section-copy mt-2">{{ $t('config.ai_recommended_copy') }}</p>
          </div>
          <div class="meta-pill shrink-0 whitespace-nowrap">
            {{ $t('config.ai_steps_done', { done: setupSteps.filter(step => step.done).length, total: setupSteps.length }) }}
          </div>
        </div>
        <div class="mt-4 grid gap-2 lg:grid-cols-4">
          <StatTile
            v-for="step in setupSteps"
            :key="step.label"
            :tile-class="step.done ? 'border-success/20 bg-success/8' : ''">
            <div class="text-xs font-semibold" :class="step.done ? 'text-success-bright' : 'text-silver'">{{ step.label }}</div>
            <div class="mt-1 text-[11px] leading-5" :class="step.done ? 'text-success-bright/80' : 'text-storm'">{{ step.status }}</div>
          </StatTile>
        </div>
      </div>
    </section>

    <div class="grid gap-6 xl:grid-cols-[1.4fr_0.9fr]">
      <div class="space-y-6">
        <section class="settings-section space-y-5">
          <div class="flex items-start justify-between gap-4">
            <div>
              <div class="section-kicker">{{ $t('config.ai_setup_kicker') }}</div>
              <h3 class="settings-section-title mt-2">{{ $t('config.ai_setup_title', { provider: currentProvider.name }) }}</h3>
              <p class="settings-section-copy mt-2">{{ $t('config.ai_setup_copy', { provider: currentProvider.name }) }}</p>
            </div>
            <div class="flex items-center gap-2">
              <button
                type="button"
                @click="resetProviderDefaults"
                class="focus-ring inline-flex items-center rounded-full border border-storm/40 px-2.5 py-1 text-[11px] font-medium text-storm transition-colors hover:border-ice/30 hover:text-silver">
                {{ $t('config.ai_reset_defaults') }}
              </button>
              <span class="inline-flex items-center rounded-full border px-2.5 py-1 text-[11px] font-medium" :class="providerPill(currentProvider.id)">
                {{ currentProvider.name }}
              </span>
            </div>
          </div>

          <div class="space-y-3">
            <div class="flex items-center justify-between gap-4">
              <div class="text-xs font-semibold uppercase tracking-eyebrow text-storm">{{ $t('config.ai_profiles') }}</div>
              <div class="text-[11px] text-storm">{{ $t('config.ai_profiles_hint') }}</div>
            </div>
            <div class="grid gap-2 sm:grid-cols-2">
              <SelectableCard
                v-for="profile in currentProfiles"
                :key="profile.id"
                :selected="isProfileActive(profile)"
                :card-class="['rounded-xl border px-4 py-3', isProfileActive(profile) ? 'border-ice/40 bg-ice/10 text-ice' : 'border-storm/40 bg-void/30 text-silver hover:border-ice/30']"
                @click="applyProviderProfile(profile)">
                <div class="text-sm font-medium">{{ profile.name }}</div>
                <div class="text-xs text-storm mt-1">{{ $t(profile.descriptionKey) }}</div>
                <div class="text-[11px] font-mono mt-2" :class="isProfileActive(profile) ? 'text-ice/90' : 'text-silver/70'">
                  {{ profile.model }}
                </div>
              </SelectableCard>
            </div>
          </div>

          <div class="space-y-3">
            <div class="text-xs font-semibold uppercase tracking-eyebrow text-storm">{{ $t('config.ai_authentication') }}</div>
            <div class="grid gap-2 sm:grid-cols-2">
              <SelectableCard
                v-for="mode in availableAuthModes"
                :key="mode"
                :selected="config.ai_auth_mode === mode"
                :card-class="['rounded-xl border px-4 py-3', config.ai_auth_mode === mode ? 'border-ice/40 bg-ice/10 text-ice' : 'border-storm/40 bg-void/30 text-silver hover:border-ice/30']"
                @click="config.ai_auth_mode = mode">
                <div class="text-sm font-medium">{{ $t(authModeLabels[mode].nameKey) }}</div>
                <div class="text-xs text-storm mt-1">{{ $t(authModeLabels[mode].descriptionKey) }}</div>
              </SelectableCard>
            </div>
          </div>

          <div class="grid gap-4 lg:grid-cols-2">
            <div>
              <div class="flex items-center justify-between gap-3 mb-1">
                <label class="block text-sm font-medium text-silver">{{ $t('config.ai_model_label') }}</label>
                <button
                  type="button"
                  @click="refreshModelCatalog()"
                  :disabled="modelsLoading || !canRefreshModels"
                  class="inline-flex items-center gap-2 rounded-full border border-storm/40 px-2.5 py-1 text-[11px] font-medium text-storm transition-colors hover:border-ice/30 hover:text-silver disabled:cursor-not-allowed disabled:opacity-50">
                  <svg v-if="modelsLoading" class="h-3.5 w-3.5 animate-spin" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
                  <span>{{ modelsLoading ? $t('config.ai_refreshing') : $t('config.ai_refresh_list') }}</span>
                </button>
              </div>
              <input
                v-model="config.ai_model"
                :list="modelOptionsId"
                type="text"
                class="settings-input font-mono text-sm"
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
                  type="button"
                  @click="config.ai_model = model.id"
                  class="inline-flex items-center rounded-full border px-2.5 py-1 text-[11px] font-medium transition-colors"
                  :class="config.ai_model === model.id ? 'border-ice/40 bg-ice/10 text-ice' : model.origin === 'live' ? 'border-success/20 bg-success/8 text-success-bright hover:border-success/35' : 'border-storm/40 bg-void/30 text-storm hover:border-ice/30 hover:text-silver'">
                  {{ model.id }}
                </button>
              </div>
              <div class="mt-2 flex items-start justify-between gap-3 text-xs">
                <div :class="modelDiscoverySummary.tone">{{ modelDiscoverySummary.text }}</div>
                <span class="inline-flex shrink-0 items-center rounded-full border px-2 py-0.5 text-[10px] font-medium uppercase tracking-eyebrow" :class="providerModelCatalog?.discovered ? 'border-success/20 bg-success/8 text-success-bright' : 'border-storm/40 bg-void/30 text-storm'">
                  {{ modelDiscoverySummary.badge }}
                </span>
              </div>
            </div>

            <div>
              <label class="block text-sm font-medium text-silver mb-1">{{ $t('config.ai_base_url_label') }}</label>
              <input
                v-model="config.ai_base_url"
                type="text"
                class="settings-input font-mono text-sm"
                :placeholder="currentProvider.defaultBaseUrl" />
              <div class="text-xs text-storm mt-1">{{ $t('config.ai_default_endpoint') }} <span class="font-mono text-silver/80">{{ currentProvider.defaultBaseUrl }}</span></div>
            </div>
          </div>

          <div v-if="config.ai_auth_mode === 'subscription'" class="rounded-xl border border-warning/20 bg-warning/6 p-4 space-y-3">
            <div>
              <div class="text-xs uppercase tracking-eyebrow text-storm">{{ currentSubscriptionLabel }}</div>
              <div class="text-sm text-silver">{{ $t('config.ai_subscription_cli_copy', { binary: currentSubscriptionBinary }) }}</div>
              <div class="text-xs text-storm mt-2">{{ $t('config.ai_subscription_test_copy') }}</div>
              <div v-if="currentSubscriptionLoginCommand" class="text-xs text-storm mt-2">
                {{ $t('config.ai_subscription_login_copy', { command: currentSubscriptionLoginCommand }) }}
              </div>
            </div>
            <div v-if="config.ai_provider === 'openai'">
              <label class="block text-sm font-medium text-silver mb-1">{{ $t('config.ai_codex_home_label') }}</label>
              <input
                v-model="config.ai_codex_home"
                type="text"
                class="settings-input font-mono text-sm"
                placeholder="~/.codex" />
              <div class="text-xs text-storm mt-1">{{ $t('config.ai_codex_home_copy') }}</div>
              <div v-if="aiStatus?.codex_home_effective" class="text-xs text-storm mt-1">{{ $t('config.ai_codex_home_effective') }} <span class="font-mono text-silver/80">{{ aiStatus.codex_home_effective }}</span></div>
            </div>
          </div>

          <div v-else-if="config.ai_auth_mode === 'none'" class="rounded-xl border border-storm/20 bg-storm/6 p-4">
            <div class="text-sm text-silver">{{ $t('config.ai_no_auth_copy') }}</div>
            <div class="text-xs text-storm mt-2">{{ $t('config.ai_no_auth_hint', { url: 'http://127.0.0.1:11434/v1' }) }}</div>
          </div>

          <div v-else>
            <label class="block text-sm font-medium text-silver mb-1">{{ $t('config.ai_api_key_label') }}</label>
            <div v-if="hasStoredApiKey" class="mb-2 flex items-center justify-between gap-3 rounded-xl border border-success/20 bg-success/8 px-3 py-2 text-xs text-success-bright">
              <span>{{ $t('config.ai_key_stored_copy') }}</span>
              <button
                type="button"
                class="rounded-full border border-success/25 px-2.5 py-1 text-[11px] font-medium text-success-bright transition-colors hover:border-danger/40 hover:text-danger-bright"
                @click="config.clear_ai_api_key = true; config.ai_api_key = ''">
                {{ $t('config.ai_key_clear') }}
              </button>
            </div>
            <div v-else-if="config.clear_ai_api_key" class="mb-2 flex items-center justify-between gap-3 rounded-xl border border-danger/20 bg-danger/8 px-3 py-2 text-xs text-danger-bright">
              <span>{{ $t('config.ai_key_will_remove') }}</span>
              <button
                type="button"
                class="rounded-full border border-danger/30 px-2.5 py-1 text-[11px] font-medium text-danger-bright transition-colors hover:border-ice/40 hover:text-ice"
                @click="config.clear_ai_api_key = false">
                {{ $t('config.ai_key_keep') }}
              </button>
            </div>
            <div class="flex gap-2">
              <div class="relative flex-1">
                <input
                  :type="showApiKey ? 'text' : 'password'"
                  v-model="config.ai_api_key"
                  @input="config.ai_api_key && (config.clear_ai_api_key = false)"
                  class="settings-input pr-10 font-mono text-sm"
                  :placeholder="currentProvider.keyPlaceholder" />
                <button type="button" :aria-label="$t('config.ai_key_toggle_aria')" @click="showApiKey = !showApiKey" class="focus-ring absolute right-2 top-1/2 -translate-y-1/2 text-storm hover:text-silver transition-colors">
                  <svg v-if="!showApiKey" class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z"/></svg>
                  <svg v-else class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13.875 18.825A10.05 10.05 0 0112 19c-4.478 0-8.268-2.943-9.543-7a9.97 9.97 0 011.563-3.029m5.858.908a3 3 0 114.243 4.243M9.878 9.878l4.242 4.242M9.88 9.88l-3.29-3.29m7.532 7.532l3.29 3.29M3 3l3.59 3.59m0 0A9.953 9.953 0 0112 5c4.478 0 8.268 2.943 9.543 7a10.025 10.025 0 01-4.132 5.411m0 0L21 21"/></svg>
                </button>
              </div>
            </div>
              <div class="text-xs text-storm mt-1">{{ $t(currentProvider.keyHintKey) }}</div>
            </div>
        </section>

        <section class="settings-section settings-section-compact space-y-4">
          <div class="flex items-center justify-between gap-3">
            <div>
              <div class="section-kicker">{{ $t('config.ai_testing_kicker') }}</div>
              <div class="settings-section-title mt-2 text-base">{{ $t('config.ai_testing_title') }}</div>
            </div>
            <button
              type="button"
              @click="testProviderConfig"
              :disabled="testLoading || aiLoading || !canTestDraft"
              class="focus-ring dashboard-action-button dashboard-action-button-primary gap-2">
              <svg v-if="testLoading || aiLoading" class="w-4 h-4 animate-spin" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
              <span>{{ testLoading || aiLoading ? $t('config.ai_testing') : $t('config.ai_test_explanation') }}</span>
            </button>
          </div>

          <div class="grid gap-4 lg:grid-cols-2">
            <div>
              <label class="block text-sm font-medium text-silver mb-1">{{ $t('config.ai_cache_duration') }}</label>
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
              <label class="block text-sm font-medium text-silver mb-1">{{ $t('config.ai_timeout_label') }}</label>
              <div class="flex items-center gap-2">
                <input
                  v-model="config.ai_timeout_ms"
                  type="number"
                  min="1000"
                  max="120000"
                  step="500"
                  class="settings-input w-32"
                  placeholder="5000" />
                <span class="text-sm text-storm">{{ $t('config.ai_timeout_unit') }}</span>
              </div>
            </div>
          </div>

          <div class="grid gap-4 lg:grid-cols-2">
            <div>
              <label class="block text-sm font-medium text-silver mb-1">{{ $t('config.ai_test_device') }}</label>
              <input
                v-model="testDeviceName"
                list="ai-device-options"
                type="text"
                class="settings-input"
                placeholder="Steam Deck OLED" />
              <div class="text-xs text-storm mt-1">{{ $t('config.ai_test_device_hint') }}</div>
              <datalist id="ai-device-options">
                <option v-for="device in aiDevices" :key="device.name" :value="device.name" />
              </datalist>
            </div>

            <div>
              <label class="block text-sm font-medium text-silver mb-1">{{ $t('config.ai_test_app') }}</label>
              <input
                v-model="testAppName"
                type="text"
                class="settings-input"
                placeholder="Rocket League" />
              <div class="text-xs text-storm mt-1">{{ $t('config.ai_test_app_hint') }}</div>
            </div>
          </div>

          <div v-if="testResult" class="rounded-xl border px-4 py-3" :class="testResult.success ? 'border-success/20 bg-success/8' : 'border-danger/20 bg-danger/8'">
            <div class="text-[10px] uppercase tracking-eyebrow" :class="testResult.success ? 'text-success' : 'text-danger'">{{ testResult.label }}</div>
            <div class="text-sm font-medium" :class="testResult.success ? 'text-success' : 'text-danger'">{{ testResult.message }}</div>
            <div v-if="testResult.detail" class="text-xs text-silver/70 mt-2">{{ testResult.detail }}</div>
            <div v-if="testResult.action" class="mt-3 rounded-lg border px-3 py-2 text-xs" :class="testResult.success ? 'border-success/20 bg-void/30 text-success-bright' : 'border-danger/20 bg-void/30 text-danger-bright'">
              {{ testResult.action }}
            </div>
            <div v-if="testResult.retryLabel" class="mt-2 text-xs font-medium text-silver/80">{{ testResult.retryLabel }}</div>
            <div v-if="testResult.success && testResult.payload" class="grid gap-2 mt-3 sm:grid-cols-2">
              <StatTile>
                <div class="stat-kicker">{{ $t('config.ai_result_source') }}</div>
                <div class="mt-2 flex flex-wrap items-center gap-2">
                  <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="cacheStatusTone(testResult.payload.cache_status)">
                    {{ optimizationSourceLabel(testResult.payload.source) }}
                  </span>
                  <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="confidenceTone(testResult.payload.confidence)">
                    {{ (testResult.payload.confidence || $t('config.ai_confidence_unknown')).toUpperCase() }}
                  </span>
                </div>
              </StatTile>
              <StatTile :label="$t('config.ai_result_authority')" :value="$t('config.ai_result_authority_copy')" />
            </div>
            <StatTile v-if="testResult.success && testResult.payload?.signals_used?.length" tile-class="mt-3">
              <div class="stat-kicker">{{ $t('config.ai_result_signals') }}</div>
              <div class="mt-2 flex flex-wrap gap-2">
                <span v-for="signal in testResult.payload.signals_used" :key="signal" class="inline-flex items-center rounded-full border border-storm/40 bg-void/30 px-2 py-0.5 text-[11px] font-medium text-silver">
                  {{ signal }}
                </span>
              </div>
            </StatTile>
            <StatTile v-if="selectedHistoryEntry" :tile-class="['mt-3', selectedHistoryEntry.consecutive_poor_outcomes > 0 ? 'border-danger/20 bg-danger/8' : '']">
              <div class="stat-kicker">{{ $t('config.ai_result_recent_outcome') }}</div>
              <div class="mt-2 flex flex-wrap items-center gap-2">
                <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="confidenceTone(selectedHistoryEntry.last_optimization_confidence)">
                  {{ optimizationSourceLabel(selectedHistoryEntry.last_optimization_source) }}
                </span>
                <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="(selectedHistoryEntry.last_quality_grade || selectedHistoryEntry.quality_grade) === 'A' || (selectedHistoryEntry.last_quality_grade || selectedHistoryEntry.quality_grade) === 'B' ? 'border-success/20 bg-success/8 text-success-bright' : (selectedHistoryEntry.last_quality_grade || selectedHistoryEntry.quality_grade) === 'C' ? 'border-warning/20 bg-warning/8 text-warning-bright' : 'border-danger/20 bg-danger/8 text-danger'">
                  {{ $t('config.ai_result_grade', { grade: selectedHistoryEntry.last_quality_grade || selectedHistoryEntry.quality_grade || '?' }) }}
                </span>
                <span class="text-xs text-storm">{{ $t('config.ai_result_updated', { when: formatRelativeTime(selectedHistoryEntry.last_updated_at) }) }}</span>
              </div>
              <div class="text-xs text-silver mt-2">
                {{ $t('config.ai_result_latest_session', {
                  fps: Math.round(selectedHistoryEntry.last_fps || selectedHistoryEntry.avg_fps || 0),
                  target: Math.round(selectedHistoryEntry.last_target_fps || selectedHistoryEntry.last_fps || selectedHistoryEntry.avg_fps || 0),
                  latency: Math.round(selectedHistoryEntry.last_latency_ms || selectedHistoryEntry.avg_latency_ms || 0),
                  kbps: selectedHistoryEntry.last_bitrate_kbps || selectedHistoryEntry.avg_bitrate_kbps || 0,
                  loss: Number(selectedHistoryEntry.last_packet_loss_pct ?? selectedHistoryEntry.packet_loss_pct ?? 0).toFixed(1),
                }) }}
              </div>
              <div class="text-xs text-silver mt-2">
                {{ $t('config.ai_result_session_totals', {
                  sessions: selectedHistoryEntry.session_count,
                  fps: Math.round(selectedHistoryEntry.avg_fps || 0),
                  poor: selectedHistoryEntry.poor_outcome_count,
                  consecutive: selectedHistoryEntry.consecutive_poor_outcomes,
                }) }}
              </div>
              <div v-if="selectedHistoryEntry.last_invalidated_at" class="text-xs text-warning-bright mt-2">
                {{ $t('config.ai_result_archived', { when: formatRelativeTime(selectedHistoryEntry.last_invalidated_at) }) }}
              </div>
            </StatTile>
          </div>
        </section>
      </div>

      <div class="space-y-6">
        <details class="settings-section settings-section-compact settings-disclosure" :open="false">
          <summary class="settings-disclosure-summary">
            <div>
              <div class="section-kicker">{{ $t('config.ai_runtime_kicker') }}</div>
              <div class="settings-section-title mt-2 text-base">{{ $t('config.ai_runtime_title') }}</div>
              <div class="settings-summary-copy">{{ $t('config.ai_runtime_copy') }}</div>
            </div>
            <div class="flex items-center gap-2">
              <span
                class="inline-flex items-center rounded-full border px-2.5 py-1 text-[11px] font-medium"
                :class="draftMatchesRuntime ? 'border-success/20 bg-success/8 text-success' : 'border-warning/20 bg-warning/8 text-warning-bright'">
                {{ draftMatchesRuntime ? $t('config.ai_in_sync') : $t('config.ai_unsaved_draft') }}
              </span>
              <svg class="settings-disclosure-chevron h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m19 9-7 7-7-7" /></svg>
            </div>
          </summary>

          <div class="settings-disclosure-body grid gap-3">
            <StatTile>
              <div class="stat-kicker">{{ $t('config.ai_runtime_provider') }}</div>
              <div class="flex items-center gap-2 mt-2">
                <span v-if="liveProvider" class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="providerPill(liveProvider.id)">
                  {{ liveProvider.name }}
                </span>
                <span class="text-sm text-silver">{{ aiStatus?.provider || $t('config.ai_not_loaded') }}</span>
              </div>
            </StatTile>

            <StatTile :label="$t('config.ai_runtime_model')">
              <div class="stat-kicker">{{ $t('config.ai_runtime_model') }}</div>
              <div class="text-sm text-silver font-mono mt-2 break-all">{{ aiStatus?.model || $t('config.ai_not_loaded') }}</div>
            </StatTile>

            <StatTile>
              <div class="stat-kicker">{{ $t('config.ai_runtime_endpoint') }}</div>
              <div class="text-sm text-silver font-mono mt-2 break-all">{{ aiStatus?.base_url || $t('config.ai_not_loaded') }}</div>
            </StatTile>

            <div class="grid gap-3 sm:grid-cols-2">
              <StatTile :label="$t('config.ai_runtime_auth')" :value="aiStatus?.auth_mode || $t('config.ai_unknown')" />
              <StatTile :label="$t('config.ai_runtime_cache_entries')" :value="String(aiStatus?.cache_count ?? 0)" />
            </div>

            <div class="grid gap-3 sm:grid-cols-2">
              <StatTile>
                <div class="stat-kicker">{{ $t('config.ai_runtime_health') }}</div>
                <div class="text-sm font-medium mt-2" :class="providerHealthSummary.tone">{{ providerHealthSummary.label }}</div>
                <div class="text-xs text-storm mt-2">{{ providerHealthSummary.detail }}</div>
              </StatTile>

              <StatTile>
                <div class="stat-kicker">{{ $t('config.ai_runtime_telemetry') }}</div>
                <div class="mt-2 grid gap-2 text-xs text-silver">
                  <div class="flex items-center justify-between gap-3">
                    <span class="text-storm">{{ $t('config.ai_runtime_last_latency') }}</span>
                    <span class="font-mono">{{ aiStatus?.last_latency_ms ?? $t('config.ai_time_never') }}<span v-if="aiStatus?.last_latency_ms != null"> ms</span></span>
                  </div>
                  <div class="flex items-center justify-between gap-3">
                    <span class="text-storm">{{ $t('config.ai_runtime_last_success') }}</span>
                    <span>{{ formatRelativeTime(aiStatus?.last_success_at) }}</span>
                  </div>
                  <div class="flex items-center justify-between gap-3">
                    <span class="text-storm">{{ $t('config.ai_runtime_last_failure') }}</span>
                    <span>{{ formatRelativeTime(aiStatus?.last_failure_at) }}</span>
                  </div>
                  <div class="flex items-center justify-between gap-3">
                    <span class="text-storm">{{ $t('config.ai_runtime_in_flight') }}</span>
                    <span class="font-mono">{{ aiStatus?.in_flight_requests ?? 0 }}</span>
                  </div>
                </div>
              </StatTile>
            </div>

            <div v-if="aiStatus?.auth_mode === 'subscription'" class="rounded-xl border border-warning/20 bg-warning/6 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">{{ liveSubscriptionLabel }}</div>
              <div class="text-sm mt-2" :class="subscriptionRuntimeTone(aiStatus)">
                {{ subscriptionRuntimeSummary(aiStatus) }}
              </div>
              <div v-if="aiStatus?.cli_authenticated === false && aiStatus?.cli_login_command" class="text-xs text-storm mt-2">
                {{ $t('config.ai_cli_authorize', { command: aiStatus.cli_login_command }) }}
              </div>
            </div>

            <div v-if="aiStatus && !draftMatchesRuntime" class="rounded-xl border border-warning/20 bg-warning/6 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">{{ $t('config.ai_pending_change') }}</div>
              <div class="text-sm text-silver mt-2">{{ $t('config.ai_pending_change_copy') }}</div>
            </div>

            <div v-if="aiStatus?.last_error" class="rounded-xl border border-danger/20 bg-danger/8 p-3">
              <div class="text-xs uppercase tracking-wider text-storm">{{ $t('config.ai_recent_error') }}</div>
              <div class="text-sm text-danger mt-2">{{ aiStatus.last_error }}</div>
            </div>
          </div>
        </details>

        <details class="settings-section settings-section-compact settings-disclosure" :open="cacheExpanded" @toggle="cacheExpanded = $event.target.open">
          <summary class="settings-disclosure-summary">
            <div class="flex items-center gap-2">
              <div class="section-kicker">{{ $t('config.ai_cache_kicker') }}</div>
              <span class="px-1.5 py-0.5 rounded text-xs font-mono bg-twilight text-silver">{{ Array.isArray(aiCache) ? aiCache.length : 0 }}</span>
            </div>
            <div class="flex items-center gap-2">
              <button
                type="button"
                @click.stop="handleClearCache"
                class="focus-ring text-xs text-storm hover:text-danger transition-colors"
                :class="{ 'opacity-50 pointer-events-none': !Array.isArray(aiCache) || aiCache.length === 0 }">
                {{ $t('config.ai_cache_clear_all') }}
              </button>
              <svg class="settings-disclosure-chevron h-4 w-4 text-storm" :class="{ 'rotate-180': cacheExpanded }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
            </div>
          </summary>
          <div class="settings-disclosure-body text-sm text-storm">{{ $t('config.ai_cache_copy') }}</div>

          <div v-if="cacheExpanded && Array.isArray(aiCache) && aiCache.length > 0" class="space-y-2 max-h-96 overflow-y-auto scrollbar-hidden">
            <div v-for="(entry, i) in aiCache" :key="i" class="py-2" :class="i > 0 ? 'border-t border-storm/20' : ''">
              <div class="flex items-center justify-between text-sm cursor-pointer" @click="entry._expanded = !entry._expanded">
                <div class="min-w-0 flex-1">
                  <div class="text-silver font-medium truncate">{{ entry.device_name }}{{ entry.app_name ? ' + ' + entry.app_name : '' }}</div>
                  <div class="text-xs text-silver/60">
                    {{ $t('config.ai_cache_explanation_only') }}<span v-if="entry.model"> · {{ entry.model }}</span>
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
                    {{ entry.cache_status || $t('config.ai_cache_stored') }}
                  </span>
                  <span class="inline-flex items-center rounded-full border px-2 py-0.5 text-[11px] font-medium" :class="confidenceTone(entry.confidence)">
                    {{ (entry.confidence || $t('config.ai_confidence_unknown')).toUpperCase() }}
                  </span>
                  <span class="text-storm">{{ $t('config.ai_cache_updated', { when: formatRelativeTime(entry.generated_at || entry.cached_at) }) }}</span>
                </div>
                <div class="flex justify-between" v-if="entry.expires_at">
                  <span class="text-storm">{{ $t('config.ai_cache_expires') }}</span>
                  <span class="text-silver">{{ formatRelativeTime(entry.expires_at) }}</span>
                </div>
                <div v-if="entry.signals_used?.length" class="pt-1.5 border-t border-storm/20">
                  <div class="text-storm mb-2">{{ $t('config.ai_result_signals') }}</div>
                  <div class="flex flex-wrap gap-1.5">
                    <span v-for="signal in entry.signals_used" :key="signal" class="inline-flex items-center rounded-full border border-storm/40 bg-void/30 px-2 py-0.5 text-[11px] font-medium text-silver">
                      {{ signal }}
                    </span>
                  </div>
                </div>
                <div v-if="entry.reasoning || entry.reasoning_summary" class="pt-1.5 border-t border-storm/20">
                  <span class="text-storm">{{ $t('config.ai_cache_explanation') }} </span>
                  <span class="text-silver/80">{{ entry.reasoning_summary || entry.reasoning }}</span>
                </div>
                <div v-if="entry.reasoning" class="pt-1.5 border-t border-storm/20">
                  <span class="text-storm">{{ $t('config.ai_cache_reasoning') }} </span>
                  <span class="text-silver/80 italic">{{ entry.reasoning }}</span>
                </div>
              </div>
            </div>
          </div>
          <div v-else-if="cacheExpanded" class="text-sm text-storm text-center py-3">{{ $t('config.ai_cache_empty') }}</div>
        </details>
      </div>
    </div>

    <details class="settings-section settings-section-compact settings-disclosure" :open="knowledgeExpanded" @toggle="knowledgeExpanded = $event.target.open">
      <summary class="settings-disclosure-summary">
        <div class="flex items-center gap-2">
          <div class="section-kicker">{{ $t('config.ai_devices_kicker') }}</div>
          <span class="px-1.5 py-0.5 rounded text-xs font-mono bg-twilight text-silver">{{ $t('config.ai_devices_count', { count: filteredDevices.length }) }}</span>
        </div>
        <svg class="settings-disclosure-chevron h-4 w-4 text-storm" :class="{ 'rotate-180': knowledgeExpanded }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
      </summary>
      <div class="settings-disclosure-body text-sm text-storm">{{ $t('config.ai_devices_copy') }}</div>
      <div v-if="knowledgeExpanded" class="mt-4">
        <input
          v-model="deviceSearch"
          @input="filterDevices"
          type="text"
          :placeholder="$t('config.ai_devices_search')"
          class="settings-input text-sm mb-3" />
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
                'bg-success/10 text-success': device.type === 'handheld',
                'bg-info/10 text-info': device.type === 'phone',
                'bg-warning/10 text-warning': device.type === 'desktop' || device.type === 'tablet'
              }">{{ device.type }}</span>
            </div>
          </div>
          <div v-if="filteredDevices.length === 0" class="text-sm text-storm text-center py-3">
            {{ deviceSearch ? $t('config.ai_devices_no_match') : $t('config.ai_devices_loading') }}
          </div>
        </div>
      </div>
    </details>
  </div>
</template>
