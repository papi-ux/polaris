<template>
  <div>
    <p class="mb-4 text-sm leading-relaxed text-storm">{{ $t('welcome.ai_intro') }}</p>
    <div class="space-y-4">
      <div>
        <div class="mb-2 text-sm font-medium text-storm">{{ $t('welcome.ai_provider_label') }}</div>
        <div class="grid gap-2 sm:grid-cols-2">
          <button
            v-for="provider in providerOptions"
            :key="provider.id"
            type="button"
            class="rounded-2xl border px-4 py-3 text-left transition-colors"
            :class="draft.ai_provider === provider.id ? 'border-ice/40 bg-ice/10 text-ice' : 'border-storm/20 bg-deep/45 text-silver hover:border-storm/40'"
            :data-provider="provider.id"
            :aria-pressed="draft.ai_provider === provider.id"
            @click="selectProvider(provider)"
          >
            <div class="text-sm font-semibold">{{ provider.name }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t(provider.summaryKey) }}</div>
          </button>
        </div>
      </div>

      <div>
        <div class="mb-2 text-sm font-medium text-storm">{{ $t('welcome.ai_auth_label') }}</div>
        <div class="flex flex-wrap gap-2">
          <button
            v-for="mode in currentProvider.authModes"
            :key="mode"
            type="button"
            class="rounded-full border px-3 py-1.5 text-xs font-medium transition-colors"
            :class="draft.ai_auth_mode === mode ? 'border-ice/40 bg-ice/10 text-ice' : 'border-storm/25 text-silver hover:border-storm/40'"
            :data-auth-mode="mode"
            :aria-pressed="draft.ai_auth_mode === mode"
            @click="selectAuthMode(mode)"
          >
            {{ $t(authModeLabels[mode].nameKey) }}
          </button>
        </div>
        <p v-if="draft.ai_auth_mode === 'subscription'" class="mt-2 text-xs text-storm">
          {{ $t('welcome.ai_auth_subscription_hint', { command: currentProvider.subscriptionLoginCommand }) }}
        </p>
        <p v-else-if="draft.ai_auth_mode === 'none'" class="mt-2 text-xs text-storm">{{ $t('welcome.ai_auth_none_hint') }}</p>
        <div v-else class="mt-2">
          <label for="welcomeAiApiKey" class="mb-1 block text-xs font-medium text-storm">{{ $t('welcome.ai_auth_key_label') }}</label>
          <input
            id="welcomeAiApiKey"
            v-model="draft.ai_api_key"
            type="password"
            autocomplete="off"
            class="settings-input font-mono text-sm"
            :placeholder="currentProvider.keyPlaceholder"
          />
          <p class="mt-1 text-xs text-storm">{{ $t(currentProvider.keyHintKey) }}</p>
        </div>
      </div>

      <div>
        <div class="mb-1 flex items-center justify-between gap-3">
          <label for="welcomeAiModel" class="block text-sm font-medium text-storm">{{ $t('welcome.ai_model_label') }}</label>
          <button
            type="button"
            class="text-xs text-storm underline-offset-4 hover:underline disabled:opacity-60"
            :disabled="modelsLoading"
            @click="refreshModels"
          >
            {{ $t('welcome.ai_refresh_models') }}
          </button>
        </div>
        <input
          id="welcomeAiModel"
          v-model="draft.ai_model"
          type="text"
          list="welcome-ai-models"
          class="settings-input font-mono text-sm"
          :placeholder="modelPlaceholder"
        />
        <datalist id="welcome-ai-models">
          <option v-for="model in modelSuggestions" :key="model.id" :value="model.id" :label="model.label" />
        </datalist>
        <div v-if="modelSuggestions.length" class="mt-2 flex flex-wrap gap-2">
          <button
            v-for="model in modelSuggestions.slice(0, 6)"
            :key="model.id"
            type="button"
            class="rounded-full border px-2.5 py-1 text-[11px] font-medium transition-colors"
            :class="draft.ai_model === model.id ? 'border-ice/40 bg-ice/10 text-ice' : 'border-storm/25 text-silver hover:border-storm/40'"
            @click="draft.ai_model = model.id"
          >
            {{ model.id }}
          </button>
        </div>
        <p class="mt-1 text-xs text-storm">{{ $t('welcome.ai_model_hint') }}</p>
        <p v-if="modelCatalogError" class="mt-1 text-xs text-warning-bright">{{ modelCatalogError }}</p>
      </div>

      <div class="flex flex-wrap items-center gap-3">
        <button
          type="button"
          class="inline-flex h-10 items-center justify-center rounded-xl border border-ice/40 bg-ice/10 px-4 text-sm font-semibold text-ice transition-colors hover:bg-ice/20 disabled:cursor-not-allowed disabled:opacity-60"
          :disabled="busy !== '' || !canTest"
          @click="runTest"
        >
          {{ busy === 'testing' ? $t('welcome.ai_testing') : $t('welcome.ai_test') }}
        </button>
        <button
          type="button"
          class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 disabled:cursor-not-allowed disabled:opacity-60"
          :disabled="busy !== '' || !testPassed"
          @click="saveAndEnable"
        >
          {{ busy === 'saving' ? $t('welcome.ai_saving') : $t('welcome.ai_save') }}
        </button>
        <button type="button" class="text-sm text-storm underline-offset-4 hover:underline" @click="$emit('skip')">
          {{ $t('welcome.skip_for_now') }}
        </button>
      </div>
      <p v-if="!testPassed" class="text-xs text-storm">{{ $t('welcome.ai_save_blocked') }}</p>

      <div
        v-if="testResult"
        class="rounded-2xl border px-4 py-3 text-sm"
        :class="testResult.ok ? 'border-success/25 bg-success/10' : 'border-danger/25 bg-danger/10'"
        role="status"
      >
        <div class="font-medium" :class="testResult.ok ? 'text-success-bright' : 'text-danger-bright'">
          {{ testResult.text || $t(testResult.textKey, testResult.params || {}) }}
        </div>
        <div v-if="testResult.detail" class="mt-1 text-xs text-silver/80">{{ testResult.detail }}</div>
        <div v-if="testResult.action" class="mt-2 text-xs text-silver">{{ testResult.action }}</div>
      </div>

      <div
        v-if="saveOutcome"
        class="rounded-2xl border px-4 py-3 text-sm"
        :class="saveOutcome.ok ? 'border-success/25 bg-success/10 text-success-bright' : 'border-danger/25 bg-danger/10 text-danger-bright'"
        role="status"
      >
        {{ $t(saveOutcome.textKey, saveOutcome.params || {}) }}
      </div>

      <p class="text-xs text-storm">
        {{ $t('welcome.ai_more_in_settings') }}
        <a href="#/config#ai_explanations" target="_blank" class="text-ice hover:underline">{{ $t('welcome.ai_open_settings') }}</a>
      </p>
    </div>
  </div>
</template>

<script setup>
import { computed, reactive, ref, watch } from 'vue'
import { useAiOptimizer } from '../composables/useAiOptimizer'
import { authModeLabels, providerDefaultTimeout, providerOptions } from '../configs/ai-provider-options.js'

const props = defineProps({
  configData: { type: Object, default: null },
  patchConfig: { type: Function, required: true },
})
const emit = defineEmits(['skip', 'saved'])

const { modelCatalog, modelsLoading, fetchModels, testConnection } = useAiOptimizer()

const initialProvider = providerOptions.find(provider => provider.id === props.configData?.ai_provider) || providerOptions[0]
const initialAuth = initialProvider.authModes.includes(props.configData?.ai_auth_mode)
  ? props.configData.ai_auth_mode
  : initialProvider.defaultAuth

const draft = reactive({
  ai_provider: initialProvider.id,
  ai_auth_mode: initialAuth,
  ai_model: props.configData?.ai_model || '',
  ai_api_key: '',
  ai_base_url: props.configData?.ai_base_url || initialProvider.defaultBaseUrl,
  ai_codex_home: props.configData?.ai_codex_home || '',
})

const busy = ref('')
const testResult = ref(null)
const saveOutcome = ref(null)
const modelCatalogError = ref('')

const currentProvider = computed(() => providerOptions.find(provider => provider.id === draft.ai_provider) || providerOptions[0])
const hasStoredKey = computed(() => !!props.configData?.has_ai_api_key)
const testPassed = computed(() => testResult.value?.ok === true)
const followsCodexCli = computed(() => draft.ai_provider === 'openai' && draft.ai_auth_mode === 'subscription')
const modelPlaceholder = computed(() => (followsCodexCli.value ? 'Codex CLI default' : currentProvider.value.defaultModel))

const canTest = computed(() => {
  if (!draft.ai_base_url) return false
  if (!draft.ai_model && !followsCodexCli.value) return false
  if (draft.ai_auth_mode === 'api_key') return !!draft.ai_api_key || hasStoredKey.value
  return true
})

const providerCatalog = computed(() => {
  const catalog = modelCatalog.value
  if (!catalog) return null
  if (catalog.provider !== draft.ai_provider || catalog.auth_mode !== draft.ai_auth_mode || catalog.base_url !== draft.ai_base_url) return null
  return catalog
})

const modelSuggestions = computed(() => {
  const seen = new Set()
  const suggestions = []
  const push = (id, label) => {
    if (!id || seen.has(id)) return
    seen.add(id)
    suggestions.push({ id, label: label || id })
  }
  const catalog = providerCatalog.value
  if (catalog?.cli_default_model) push(catalog.cli_default_model)
  ;(catalog?.models || []).forEach(model => push(model.id, model.label))
  ;(catalog?.fallback_models || []).forEach(model => push(model.id, model.label))
  if (!catalog && !followsCodexCli.value) push(currentProvider.value.defaultModel)
  return suggestions
})

function resetOutcome() {
  testResult.value = null
  saveOutcome.value = null
}

function selectProvider(provider) {
  if (draft.ai_provider === provider.id) return
  draft.ai_provider = provider.id
  draft.ai_auth_mode = provider.defaultAuth
  draft.ai_base_url = provider.defaultBaseUrl
  draft.ai_api_key = ''
  draft.ai_model = provider.id === 'openai' && provider.defaultAuth === 'subscription' ? '' : provider.defaultModel
  resetOutcome()
}

function selectAuthMode(mode) {
  if (draft.ai_auth_mode === mode) return
  draft.ai_auth_mode = mode
  if (draft.ai_provider === 'openai') {
    // Codex and the hosted API accept different model names; let the list decide.
    draft.ai_model = mode === 'subscription' ? '' : currentProvider.value.defaultModel
  }
  resetOutcome()
}

function draftPayload() {
  return {
    ai_provider: draft.ai_provider,
    ai_model: draft.ai_model,
    ai_auth_mode: draft.ai_auth_mode,
    ai_api_key: draft.ai_api_key || '',
    clear_ai_api_key: false,
    ai_base_url: draft.ai_base_url,
    ai_use_subscription: draft.ai_auth_mode === 'subscription' ? 'enabled' : 'disabled',
    ai_codex_home: draft.ai_codex_home || '',
    ai_timeout_ms: providerDefaultTimeout(currentProvider.value, draft.ai_auth_mode),
    ai_cache_ttl_hours: 168,
  }
}

async function refreshModels() {
  modelCatalogError.value = ''
  if (draft.ai_auth_mode === 'api_key' && !draft.ai_api_key && !hasStoredKey.value) return
  const result = await fetchModels(draftPayload())
  if (!result) return
  // The tab pre-fills nothing here; an empty model follows the Codex CLI's own default.
  if (result.source === 'codex_cli' && result.cli_default_model && followsCodexCli.value && !draft.ai_model) {
    draft.ai_model = result.cli_default_model
  }
  if (!result.discovered && result.error) {
    modelCatalogError.value = result.error
  }
}

watch(() => [draft.ai_provider, draft.ai_auth_mode, draft.ai_base_url], () => {
  refreshModels()
}, { immediate: true })

async function runTest() {
  busy.value = 'testing'
  resetOutcome()
  try {
    const result = await testConnection(draftPayload(), 'Steam Deck OLED', 'Rocket League')
    if (result?.status) {
      testResult.value = { ok: true, textKey: 'welcome.ai_test_ok', params: { provider: currentProvider.value.name }, detail: result.reasoning || '' }
    } else {
      testResult.value = { ok: false, text: result?.error || '', textKey: 'welcome.ai_test_failed', detail: result?.detail || '', action: result?.action || '' }
    }
  } finally {
    busy.value = ''
  }
}

async function saveAndEnable() {
  if (!testPassed.value) return
  busy.value = 'saving'
  saveOutcome.value = null
  try {
    const body = {
      ai_enabled: 'enabled',
      ai_provider: draft.ai_provider,
      ai_auth_mode: draft.ai_auth_mode,
      ai_base_url: draft.ai_base_url,
      ai_use_subscription: draft.ai_auth_mode === 'subscription' ? 'enabled' : 'disabled',
      ai_timeout_ms: providerDefaultTimeout(currentProvider.value, draft.ai_auth_mode),
    }
    if (draft.ai_model) body.ai_model = draft.ai_model
    if (draft.ai_api_key) body.ai_api_key = draft.ai_api_key
    if (draft.ai_codex_home) body.ai_codex_home = draft.ai_codex_home
    const saved = await props.patchConfig(body)
    if (!saved.ok) {
      saveOutcome.value = { ok: false, textKey: 'welcome.ai_save_failed', params: { error: saved.error || '' } }
      return
    }
    saveOutcome.value = { ok: true, textKey: saved.restartRequired === false ? 'welcome.ai_saved' : 'welcome.ai_saved_restart' }
    emit('saved', saved)
  } finally {
    busy.value = ''
  }
}
</script>
