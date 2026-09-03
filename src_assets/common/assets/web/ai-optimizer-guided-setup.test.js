import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'

// The tab renders through the injected translator; resolve the real English
// locale here so the assertions below keep reading the shipped copy.
const locale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))
function translate(key, params = {}) {
  const value = key.split('.').reduce((node, part) => (node && typeof node === 'object' ? node[part] : undefined), locale)
  if (typeof value !== 'string') return key
  return value.replace(/\{(\w+)\}/g, (match, name) => (name in params ? String(params[name]) : match))
}

import AiOptimizer from './configs/tabs/AiOptimizer.vue'

const mockState = {
  status: ref(null),
  cache: ref([]),
  history: ref([]),
  devices: ref([]),
  loading: ref(false),
  modelCatalog: ref(null),
  modelsLoading: ref(false),
}

const mockAiOptimizer = {
  fetchStatus: vi.fn(() => Promise.resolve(mockState.status.value)),
  fetchCache: vi.fn(() => Promise.resolve(mockState.cache.value)),
  fetchHistory: vi.fn(() => Promise.resolve(mockState.history.value)),
  fetchDevices: vi.fn(() => Promise.resolve(mockState.devices.value)),
  fetchModels: vi.fn(() => Promise.resolve(mockState.modelCatalog.value)),
  clearCache: vi.fn(() => Promise.resolve(true)),
  testConnection: vi.fn(() => Promise.resolve({ status: true })),
}

const mockToast = vi.fn()

vi.mock('./composables/useAiOptimizer', () => ({
  useAiOptimizer: () => ({
    ...mockState,
    ...mockAiOptimizer,
  }),
}))

vi.mock('./composables/useToast', () => ({
  useToast: () => ({ toast: mockToast }),
}))

function defaultConfig(overrides = {}) {
  return {
    ai_enabled: 'disabled',
    adaptive_bitrate_enabled: 'disabled',
    ai_provider: 'anthropic',
    ai_model: 'claude-haiku-4-5-20251001',
    ai_auth_mode: 'subscription',
    ai_api_key: '',
    clear_ai_api_key: false,
    has_ai_api_key: false,
    ai_base_url: 'https://api.anthropic.com',
    ai_use_subscription: 'enabled',
    ai_codex_home: '',
    ai_timeout_ms: 5000,
    ai_cache_ttl_hours: 168,
    ...overrides,
  }
}

async function flushMounted() {
  await Promise.resolve()
  await Promise.resolve()
  await nextTick()
}

function mountOptimizer(config = defaultConfig()) {
  return mount(AiOptimizer, {
    props: { config },
    attachTo: document.body,
    global: { provide: { i18n: { t: translate } } },
  })
}

describe('AI optimizer guided setup', () => {
  beforeEach(() => {
    document.body.innerHTML = ''
    vi.clearAllMocks()
    mockState.status.value = {
      enabled: false,
      provider: 'anthropic',
      model: 'claude-haiku-4-5-20251001',
      auth_mode: 'subscription',
      base_url: 'https://api.anthropic.com',
      cli_available: true,
      cli_authenticated: false,
      cli_login_command: 'claude login',
      subscription_cli: 'Claude CLI',
      cache_count: 0,
      in_flight_requests: 0,
    }
    mockState.cache.value = []
    mockState.history.value = []
    mockState.devices.value = [{ name: 'Steam Deck OLED', type: 'handheld' }]
    mockState.loading.value = false
    mockState.modelCatalog.value = null
    mockState.modelsLoading.value = false
    mockAiOptimizer.testConnection.mockResolvedValue({ status: true })
  })

  it('surfaces a recommended setup checklist and provider cards with auth plus runtime state', async () => {
    const config = defaultConfig()
    const wrapper = mountOptimizer(config)
    await flushMounted()

    const text = wrapper.text()
    expect(text).toContain('Recommended setup')
    expect(text).toContain('1. Choose provider')
    expect(text).toContain('2. Verify auth')
    expect(text).toContain('3. Test draft')
    expect(text).toContain('4. Enable explanations')
    expect(text).not.toContain('Build first stream profile')
    expect(text).toContain('Auth: Subscription, API Key')
    expect(text).toContain('Saved runtime')
    expect(text).toContain('Run claude login')
  })

  it('applies provider-card selection as the first guided setup step', async () => {
    const config = defaultConfig()
    const wrapper = mountOptimizer(config)
    await flushMounted()

    const openAiCard = wrapper.findAll('button').find(button => button.text().includes('OpenAI'))
    expect(openAiCard).toBeTruthy()

    await openAiCard.trigger('click')
    await nextTick()

    expect(config.ai_provider).toBe('openai')
    expect(config.ai_model).toBe('gpt-5.4-mini')
    expect(config.ai_base_url).toBe('https://api.openai.com/v1')
    expect(config.ai_auth_mode).toBe('subscription')
    expect(wrapper.text()).toContain('Guided step: choose the OpenAI setup that matches your auth')
  })

  it('uses a realistic bounded timeout when selecting a local provider profile', async () => {
    const config = defaultConfig()
    const wrapper = mountOptimizer(config)
    await flushMounted()

    const localCard = wrapper.findAll('button').find(button => button.text().includes('Local'))
    expect(localCard).toBeTruthy()
    await localCard.trigger('click')
    await nextTick()

    const ollamaProfile = wrapper.findAll('button').find(button => button.text().includes('Ollama'))
    expect(ollamaProfile).toBeTruthy()
    await ollamaProfile.trigger('click')
    await nextTick()

    expect(config.ai_provider).toBe('local')
    expect(config.ai_auth_mode).toBe('none')
    expect(config.ai_timeout_ms).toBe(60000)
    expect(wrapper.find('input[type="number"][max="120000"]').exists()).toBe(true)
  })

  it('renders failed draft tests as structured actionable feedback', async () => {
    const config = defaultConfig({
      ai_provider: 'openai',
      ai_model: 'gpt-5.4-mini',
      ai_base_url: 'https://api.openai.com/v1',
      ai_auth_mode: 'subscription',
      ai_use_subscription: 'enabled',
    })
    mockAiOptimizer.testConnection.mockResolvedValue({
      status: false,
      error: 'codex CLI is not authorized for this runtime HOME',
      action: 'Run codex login or set CODEX_HOME to the signed-in account home.',
      retryable: true,
    })

    const wrapper = mountOptimizer(config)
    await flushMounted()

    const testButton = wrapper.findAll('button').find(button => button.text().includes('Test explanation'))
    expect(testButton).toBeTruthy()

    await testButton.trigger('click')
    await flushMounted()

    const text = wrapper.text()
    expect(text).toContain('Action needed')
    expect(text).toContain('codex CLI is not authorized for this runtime HOME')
    expect(text).toContain('Run codex login or set CODEX_HOME to the signed-in account home.')
    expect(text).toContain('Retry after fixing auth')
  })

  it('explains a local inference timeout without blaming authentication', async () => {
    const config = defaultConfig({
      ai_provider: 'local',
      ai_model: 'qwen3.8:27b',
      ai_base_url: 'http://127.0.0.1:11434/v1',
      ai_auth_mode: 'none',
      ai_use_subscription: 'disabled',
      ai_timeout_ms: 5000,
    })
    mockAiOptimizer.testConnection.mockResolvedValue({
      status: false,
      code: 'inference_timeout',
      error: 'Model inference timed out after 60 seconds',
      detail: 'Large local models may still be loading.',
      action: 'Warm the model once, or raise Provider timeout to 60000 ms, then retry.',
      retryable: true,
    })

    const wrapper = mountOptimizer(config)
    await flushMounted()

    const testButton = wrapper.findAll('button').find(button => button.text().includes('Test explanation'))
    expect(testButton).toBeTruthy()
    await testButton.trigger('click')
    await flushMounted()

    const text = wrapper.text()
    expect(config.ai_timeout_ms).toBe(60000)
    expect(text).toContain('Model inference timed out after 60 seconds')
    expect(text).toContain('Large local models may still be loading.')
    expect(text).toContain('raise Provider timeout to 60000 ms')
    expect(text).toContain('Retry test')
    expect(text).not.toContain('Retry after fixing auth')
  })
})
