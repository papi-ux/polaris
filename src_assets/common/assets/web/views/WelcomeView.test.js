import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import WelcomeView from './WelcomeView.vue'

function response(status, body = {}) {
  return {
    json: vi.fn(async () => body),
    ok: status >= 200 && status < 300,
    status,
  }
}

function mountWelcome() {
  return mount(WelcomeView, {
    global: {
      mocks: {
        $t: (key) => key,
      },
      stubs: {
        ResourceCard: true,
      },
    },
  })
}

async function submitCredentials(wrapper) {
  await wrapper.get('#passwordInput').setValue('test-password')
  await wrapper.get('#confirmPasswordInput').setValue('test-password')
  await wrapper.get('form').trigger('submit')
  await flushPromises()
}

describe('WelcomeView credential setup', () => {
  beforeEach(() => {
    vi.stubGlobal('fetch', vi.fn())
    window.history.replaceState(null, '', '/#/welcome')
  })

  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('routes a preserved-credential rejection to sign in', async () => {
    fetch.mockResolvedValueOnce(response(401, { status: false, error: 'Unauthorized' }))
    const wrapper = mountWelcome()

    await submitCredentials(wrapper)

    expect(window.location.hash).toBe('#/login')
    expect(wrapper.text()).not.toContain('Internal Server Error')
    wrapper.unmount()
  })

  it('shows the typed server error when first-run persistence fails', async () => {
    fetch.mockResolvedValueOnce(response(500, {
      status: false,
      error: 'Credentials could not be persisted',
    }))
    const wrapper = mountWelcome()

    await submitCredentials(wrapper)

    expect(wrapper.text()).toContain('Credentials could not be persisted')
    expect(wrapper.text()).not.toContain('Internal Server Error')
    wrapper.unmount()
  })

  it('distinguishes a connection failure from a server rejection', async () => {
    fetch.mockRejectedValueOnce(new TypeError('Failed to fetch'))
    const wrapper = mountWelcome()

    await submitCredentials(wrapper)

    expect(wrapper.text()).toContain('Could not reach Polaris')
    wrapper.unmount()
  })
})

describe('WelcomeView optional setup steps', () => {
  const codexCatalog = {
    status: true,
    provider: 'openai',
    auth_mode: 'subscription',
    base_url: 'https://api.openai.com/v1',
    discovered: true,
    source: 'codex_cli',
    cli_default_model: 'gpt-5.6-sol',
    model_count: 2,
    models: [{ id: 'gpt-6-astra', label: 'GPT-6-Astra' }, { id: 'gpt-5.6-sol', label: 'GPT-5.6-Sol' }],
    fallback_models: [],
  }
  const claudeCatalog = {
    status: true,
    provider: 'anthropic',
    auth_mode: 'subscription',
    base_url: 'https://api.anthropic.com',
    discovered: false,
    models: [],
    fallback_models: [{ id: 'claude-haiku-4-5-20251001' }],
  }

  function baseRoutes() {
    return {
      'POST ./api/password': () => response(200, { status: true }),
      'GET ./api/config': () => response(200, {
        configuration_revision: 'r1',
        has_steamgriddb_api_key: false,
        has_ai_api_key: false,
        port: 47989,
      }),
      'POST ./api/ai/models': (init) => {
        const body = JSON.parse(init.body)
        return response(200, body.ai_provider === 'openai' ? codexCatalog : claudeCatalog)
      },
    }
  }

  function routeFetch(routes) {
    fetch.mockImplementation(async (url, init = {}) => {
      const method = init.method || 'GET'
      const handler = routes[`${method} ${url}`]
      if (!handler) return response(404, { status: false, error: `unrouted ${method} ${url}` })
      return handler(init)
    })
  }

  function calls() {
    return fetch.mock.calls.map(([url, init = {}]) => ({
      url,
      method: init.method || 'GET',
      headers: init.headers || {},
      body: init.body ? JSON.parse(init.body) : null,
    }))
  }

  function button(wrapper, text) {
    return wrapper.findAll('button').find(candidate => candidate.text() === text)
  }

  async function settle() {
    await flushPromises()
    await flushPromises()
    await flushPromises()
  }

  async function reachStep(wrapper, index) {
    await submitCredentials(wrapper)
    for (let step = 0; step < index; step += 1) {
      await button(wrapper, 'Next').trigger('click')
      await settle()
    }
  }

  beforeEach(() => {
    vi.stubGlobal('fetch', vi.fn())
    window.history.replaceState(null, '', '/#/welcome')
  })

  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('lists seven steps, with artwork and AI explanations before pairing', async () => {
    routeFetch(baseRoutes())
    const wrapper = mountWelcome()
    await settle()

    const text = wrapper.text()
    expect(text).toContain('Step 1 of 7')
    for (const title of ['Credentials', 'GPU Detection', 'Network', 'First App', 'welcome.step_artwork', 'welcome.step_ai', 'Pair Client']) {
      expect(text).toContain(title)
    }
    const order = ['First App', 'welcome.step_artwork', 'welcome.step_ai', 'Pair Client'].map(title => text.indexOf(title))
    expect([...order].sort((a, b) => a - b)).toEqual(order)
    wrapper.unmount()
  })

  it('checks a SteamGridDB key before saving it and uses it without a restart', async () => {
    const routes = baseRoutes()
    routes['POST ./api/covers/key/check'] = () => response(200, { status: true, code: 'steamgriddb_ok', matches: 3 })
    routes['PATCH ./api/config'] = () => response(200, { status: true, configuration_revision: 'r2', restart_required: false })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 4)
    expect(wrapper.text()).toContain('Step 5 of 7')

    await wrapper.get('#welcomeSteamGridDbKey').setValue('sgdb-test-key')
    await button(wrapper, 'welcome.artwork_check_and_save').trigger('click')
    await settle()

    const made = calls()
    const check = made.find(call => call.url === './api/covers/key/check')
    expect(check.method).toBe('POST')
    expect(check.body).toEqual({ steamgriddb_api_key: 'sgdb-test-key' })
    const patch = made.find(call => call.method === 'PATCH' && call.url === './api/config')
    expect(patch.body).toEqual({ steamgriddb_api_key: 'sgdb-test-key' })
    expect(patch.headers['If-Match']).toBe('"r1"')
    expect(made.indexOf(check)).toBeLessThan(made.indexOf(patch))

    const text = wrapper.text()
    expect(text).toContain('welcome.artwork_ok')
    expect(text).toContain('welcome.artwork_saved')
    expect(wrapper.get('#welcomeSteamGridDbKey').element.value).toBe('')
    expect(wrapper.html()).not.toContain('sgdb-test-key')

    await button(wrapper, 'Next').trigger('click')
    await settle()
    await button(wrapper, 'Next').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('Step 7 of 7')
    expect(wrapper.text()).not.toContain('welcome.restart_needed_one')
    expect(button(wrapper, 'welcome.restart_now')).toBeFalsy()
    wrapper.unmount()
  })

  it('still offers the restart when the host says a saved key waits for one', async () => {
    const routes = baseRoutes()
    routes['POST ./api/covers/key/check'] = () => response(200, { status: true, code: 'steamgriddb_ok', matches: 1 })
    routes['PATCH ./api/config'] = () => response(200, { status: true, configuration_revision: 'r2' })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 4)

    await wrapper.get('#welcomeSteamGridDbKey').setValue('sgdb-test-key')
    await button(wrapper, 'welcome.artwork_check_and_save').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('welcome.artwork_saved_restart')

    await button(wrapper, 'Next').trigger('click')
    await settle()
    await button(wrapper, 'Next').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('Step 7 of 7')
    expect(wrapper.text()).toContain('welcome.restart_needed_one')
    expect(button(wrapper, 'welcome.restart_now')).toBeTruthy()
    wrapper.unmount()
  })

  it('shows why SteamGridDB rejected a key and saves nothing', async () => {
    const routes = baseRoutes()
    routes['POST ./api/covers/key/check'] = () => response(502, {
      status: false,
      code: 'steamgriddb_unauthorized',
      error: "SteamGridDB rejected the host's API key. Update it in Polaris settings.",
    })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 4)

    await wrapper.get('#welcomeSteamGridDbKey').setValue('bad-key')
    await button(wrapper, 'welcome.artwork_check_and_save').trigger('click')
    await settle()

    expect(wrapper.text()).toContain("SteamGridDB rejected the host's API key.")
    expect(wrapper.text()).toContain('welcome.artwork_hint_unauthorized')
    expect(calls().some(call => call.method === 'PATCH')).toBe(false)
    wrapper.unmount()
  })

  it('adopts the Codex CLI model, gates Save on a passing test and saves the provider', async () => {
    const routes = baseRoutes()
    routes['POST ./api/ai/test'] = () => response(200, { status: true, reasoning: 'Evidence explained.' })
    routes['PATCH ./api/config'] = () => response(200, { status: true, configuration_revision: 'r3' })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 5)
    expect(wrapper.text()).toContain('Step 6 of 7')
    expect(button(wrapper, 'welcome.ai_save').attributes('disabled')).toBeDefined()

    await wrapper.get('[data-provider="openai"]').trigger('click')
    await settle()
    expect(wrapper.get('#welcomeAiModel').element.value).toBe('gpt-5.6-sol')

    await button(wrapper, 'welcome.ai_test').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('welcome.ai_test_ok')
    expect(wrapper.text()).toContain('Evidence explained.')
    expect(button(wrapper, 'welcome.ai_save').attributes('disabled')).toBeUndefined()

    await button(wrapper, 'welcome.ai_save').trigger('click')
    await settle()
    const patch = calls().find(call => call.method === 'PATCH' && call.url === './api/config')
    expect(patch.body).toMatchObject({
      ai_enabled: 'enabled',
      ai_provider: 'openai',
      ai_auth_mode: 'subscription',
      ai_model: 'gpt-5.6-sol',
      ai_use_subscription: 'enabled',
      ai_base_url: 'https://api.openai.com/v1',
    })
    expect(patch.body.ai_api_key).toBeUndefined()
    expect(wrapper.text()).toContain('welcome.ai_saved')
    wrapper.unmount()
  })

  it('keeps Save off and shows the provider reason when the test fails', async () => {
    const routes = baseRoutes()
    routes['POST ./api/ai/test'] = () => response(200, {
      status: false,
      code: 'codex_cli_rejected',
      error: 'Codex refused the request',
      detail: "The 'gpt-5.4-mini' model is not supported when using Codex with a ChatGPT account.",
      action: 'Pick a model from the list, which comes from your Codex CLI, or run codex login, then retry.',
      retryable: true,
    })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 5)
    await wrapper.get('[data-provider="openai"]').trigger('click')
    await settle()

    await button(wrapper, 'welcome.ai_test').trigger('click')
    await settle()

    const text = wrapper.text()
    expect(text).toContain('Codex refused the request')
    expect(text).toContain("The 'gpt-5.4-mini' model is not supported when using Codex with a ChatGPT account.")
    expect(text).toContain('Pick a model from the list')
    expect(button(wrapper, 'welcome.ai_save').attributes('disabled')).toBeDefined()
    expect(calls().some(call => call.method === 'PATCH')).toBe(false)
    wrapper.unmount()
  })

  it('skips both optional steps without writing anything', async () => {
    routeFetch(baseRoutes())
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 4)

    await button(wrapper, 'welcome.skip_for_now').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('Step 6 of 7')
    await button(wrapper, 'welcome.skip_for_now').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('Step 7 of 7')
    expect(wrapper.text()).not.toContain('welcome.restart_now')
    expect(calls().some(call => call.method === 'PATCH')).toBe(false)
    wrapper.unmount()
  })
})
