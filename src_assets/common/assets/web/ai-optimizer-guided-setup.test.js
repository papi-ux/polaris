import { mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'

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
    expect(text).toContain('4. Enable Auto Quality')
    expect(text).toContain('5. Build first stream profile')
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

    const testButton = wrapper.findAll('button').find(button => button.text().includes('Test draft'))
    expect(testButton).toBeTruthy()

    await testButton.trigger('click')
    await flushMounted()

    const text = wrapper.text()
    expect(text).toContain('Action needed')
    expect(text).toContain('codex CLI is not authorized for this runtime HOME')
    expect(text).toContain('Run codex login or set CODEX_HOME to the signed-in account home.')
    expect(text).toContain('Retry after fixing auth')
  })
})
