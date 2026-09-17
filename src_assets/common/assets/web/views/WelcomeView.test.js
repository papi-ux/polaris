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

  const modeOptions = [
    { value: 'headless_stream', available: true },
    { value: 'windowed_stream', available: true },
    { value: 'gamescope_stream', available: false, unavailable_reason: 'gamescope is not installed.' },
    { value: 'headless_dongle', available: true },
    { value: 'desktop_display', available: true },
  ]

  const nvidiaHardware = {
    status: true,
    platform: 'linux',
    gpus: [{
      render_node: '/dev/dri/renderD128',
      vendor: 'nvidia',
      model: 'NVIDIA GeForce RTX 4090',
      driver: 'nvidia',
      driver_version: '610.57.04',
      selected: true,
      vaapi: null,
    }],
    build: { cuda: true, vaapi: true },
    encoder: { configured: '', policy: 'nvidia_nvenc', planned: 'nvenc', active: '', expected: 'nvenc' },
    encoder_choices: ['nvenc'],
    nvenc_min_driver: '570',
    advice: [{ code: 'encoder_confirmed_at_first_stream', severity: 'info', render_node: '', commands: [], params: {} }],
  }

  function baseRoutes(config = {}) {
    return {
      'POST ./api/password': () => response(200, { status: true }),
      'GET ./api/config': () => response(200, {
        configuration_revision: 'r1',
        has_steamgriddb_api_key: false,
        has_ai_api_key: false,
        port: 47989,
        platform: 'linux',
        encoder: '',
        linux_stream_mode: 'headless_stream',
        stream_display_mode_options: modeOptions,
        trusted_subnets: '',
        trusted_subnet_auto_pairing: 'disabled',
        ...config,
      }),
      'GET ./api/setup/hardware': () => response(200, nvidiaHardware),
      'GET ./api/setup/networks': () => response(200, {
        status: true,
        supported: true,
        networks: [{ interface: 'eno2', interfaces: ['eno2'], cidr: '10.0.0.0/24', address: '10.0.0.232' }],
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

  async function reachLast(wrapper) {
    while (button(wrapper, 'Next')) {
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

  it('lists eight steps in order, with First App last', async () => {
    routeFetch(baseRoutes())
    const wrapper = mountWelcome()
    await settle()

    const text = wrapper.text()
    expect(text).toContain('Step 1 of 8')
    const titles = ['Credentials', 'welcome.step_gpu', 'welcome.step_launch_mode', 'Network', 'welcome.step_artwork', 'welcome.step_ai', 'Pair Client', 'First App']
    const order = titles.map(title => text.indexOf(title))
    expect(order.every(index => index >= 0)).toBe(true)
    expect([...order].sort((a, b) => a - b)).toEqual(order)
    wrapper.unmount()
  })

  it('leaves Launch Mode out on a host that is not Linux', async () => {
    routeFetch(baseRoutes({ platform: 'windows' }))
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 1)

    const text = wrapper.text()
    expect(text).toContain('Step 2 of 7')
    expect(text).not.toContain('welcome.step_launch_mode')
    wrapper.unmount()
  })

  it('shows the GPU, the encoder and why, and saves an encoder choice', async () => {
    const routes = baseRoutes()
    routes['PATCH ./api/config'] = () => response(200, { status: true, configuration_revision: 'r2', restart_required: true })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 1)

    let text = wrapper.text()
    expect(text).toContain('Step 2 of 8')
    expect(text).toContain('NVIDIA GeForce RTX 4090')
    expect(text).toContain('welcome.gpu_nvidia_floor')
    expect(text).toContain('welcome.gpu_cuda_yes')
    expect(text).toContain('welcome.encoder_nvenc')
    expect(text).toContain('welcome.gpu_reason_nvidia')
    expect(text).toContain('welcome.gpu_encoder_state_expected')
    expect(wrapper.find('[data-advice="encoder_confirmed_at_first_stream"]').exists()).toBe(false)
    const options = wrapper.findAll('#welcomeEncoder option').map(option => option.element.value)
    expect(options).toEqual(['', 'nvenc'])
    expect(button(wrapper, 'welcome.gpu_choice_save').attributes('disabled')).toBeDefined()

    await wrapper.get('#welcomeEncoder').setValue('nvenc')
    await button(wrapper, 'welcome.gpu_choice_save').trigger('click')
    await settle()

    const patch = calls().find(call => call.method === 'PATCH' && call.url === './api/config')
    expect(patch.body).toEqual({ encoder: 'nvenc' })
    expect(patch.headers['If-Match']).toBe('"r1"')
    text = wrapper.text()
    expect(text).toContain('welcome.gpu_choice_saved_restart')

    await reachLast(wrapper)
    expect(wrapper.text()).toContain('welcome.restart_needed_one')
    wrapper.unmount()
  })

  it('tells an AMD host on stock Fedora Mesa what is missing and how to fix it', async () => {
    const routes = baseRoutes()
    routes['GET ./api/setup/hardware'] = () => response(200, {
      status: true,
      gpus: [{
        render_node: '/dev/dri/renderD128',
        vendor: 'amd',
        model: 'Navi 31 [Radeon RX 7900 XT/7900 XTX/7900 GRE/7900M]',
        driver: 'amdgpu',
        driver_version: '',
        selected: true,
        vaapi: { driver_loaded: true, driver_vendor: 'Mesa Gallium driver 25.1.9 for AMD Radeon RX 7900 XTX', h264: false, hevc: false, av1: true },
      }],
      build: { cuda: true, vaapi: true },
      encoder: { configured: '', policy: 'amd_established_desktop', planned: 'vaapi', active: '', expected: 'software' },
      encoder_choices: [],
      nvenc_min_driver: '570',
      advice: [
        {
          code: 'amd_vaapi_encode_missing_fedora',
          severity: 'fail',
          render_node: '/dev/dri/renderD128',
          commands: [
            'sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm',
            'sudo dnf swap mesa-va-drivers mesa-va-drivers-freeworld',
          ],
          params: {},
        },
        { code: 'vaapi_av1_only', severity: 'info', render_node: '/dev/dri/renderD128', commands: [], params: {} },
        { code: 'encoder_confirmed_at_first_stream', severity: 'info', render_node: '', commands: [], params: {} },
      ],
    })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 1)

    const advice = wrapper.get('[data-advice="amd_vaapi_encode_missing_fedora"]')
    expect(advice.text()).toContain('welcome.gpu_advice_amd_vaapi_encode_missing_fedora')
    expect(advice.get('pre').text()).toBe(
      'sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm\n'
      + 'sudo dnf swap mesa-va-drivers mesa-va-drivers-freeworld'
    )
    expect(wrapper.find('[data-advice="vaapi_av1_only"]').exists()).toBe(true)
    expect(wrapper.find('[data-codec="h264"]').text()).toBe('welcome.gpu_codec_no')
    expect(wrapper.find('[data-codec="av1"]').text()).toBe('welcome.gpu_codec_yes')
    expect(wrapper.text()).toContain('welcome.encoder_software')
    expect(wrapper.text()).toContain('welcome.gpu_reason_software')
    expect(wrapper.findAll('#welcomeEncoder option').map(option => option.element.value)).toEqual([''])
    wrapper.unmount()
  })

  it('preselects the current launch mode and saves a new one with the keys Settings writes', async () => {
    const routes = baseRoutes()
    routes['PATCH ./api/config'] = () => response(200, { status: true, configuration_revision: 'r2', restart_required: true })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 2)

    const text = wrapper.text()
    expect(text).toContain('Step 3 of 8')
    expect(text).toContain('welcome.launch_mode_per_game')
    expect(text).toContain('welcome.launch_mode_nvidia_hint')
    const private_stream = wrapper.get('[data-mode="headless_stream"]')
    expect(private_stream.attributes('aria-checked')).toBe('true')
    expect(private_stream.text()).toContain('welcome.launch_mode_recommended')
    expect(private_stream.text()).toContain('welcome.launch_mode_current')
    expect(wrapper.get('[data-mode="gamescope_stream"]').attributes('disabled')).toBeDefined()
    expect(wrapper.get('[data-mode="headless_dongle"]').attributes('disabled')).toBeDefined()
    expect(button(wrapper, 'welcome.launch_mode_save').attributes('disabled')).toBeDefined()

    await wrapper.get('[data-mode="windowed_stream"]').trigger('click')
    await button(wrapper, 'welcome.launch_mode_save').trigger('click')
    await settle()

    const patch = calls().find(call => call.method === 'PATCH' && call.url === './api/config')
    expect(patch.body).toEqual({
      linux_stream_mode: 'windowed_stream',
      linux_private_runtime: 'labwc',
      capture: 'wlr',
      linux_auto_manage_displays: 'disabled',
      headless_swap_mode: '',
      headless_mode: 'enabled',
      linux_use_cage_compositor: 'enabled',
      linux_prefer_gpu_native_capture: 'enabled',
    })
    expect(patch.headers['If-Match']).toBe('"r1"')
    expect(wrapper.text()).toContain('welcome.launch_mode_saved_restart')
    expect(wrapper.get('[data-mode="windowed_stream"]').text()).toContain('welcome.launch_mode_current')

    await reachLast(wrapper)
    expect(wrapper.text()).toContain('welcome.restart_needed_one')
    expect(button(wrapper, 'welcome.restart_now')).toBeTruthy()
    wrapper.unmount()
  })

  it('trusts the detected network and a typed one, keeping what was trusted before', async () => {
    const routes = baseRoutes({ trusted_subnets: '["192.168.50.0/24"]', trusted_subnet_auto_pairing: 'disabled' })
    let revision = 1
    routes['PATCH ./api/config'] = () => {
      revision += 1
      return response(200, { status: true, configuration_revision: `r${revision}`, restart_required: false })
    }
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 3)

    expect(wrapper.text()).toContain('Step 4 of 8')
    expect(wrapper.text()).toContain('UDP: 47998-48000, 48002, 48010')
    expect(wrapper.text()).toContain('welcome.network_trusted_warning')
    const detected = wrapper.get('[data-network="10.0.0.0/24"]')
    expect(detected.text()).toContain('welcome.network_on_interface')

    await detected.get('button').trigger('click')
    await settle()
    let patches = calls().filter(call => call.method === 'PATCH' && call.url === './api/config')
    expect(patches).toHaveLength(1)
    expect(patches[0].body).toEqual({
      trusted_subnets: '192.168.50.0/24,10.0.0.0/24',
      trusted_subnet_auto_pairing: 'enabled',
    })
    expect(patches[0].headers['If-Match']).toBe('"r1"')
    expect(wrapper.text()).toContain('welcome.network_trust_saved')
    expect(wrapper.get('[data-network="10.0.0.0/24"] button').text()).toBe('welcome.network_already_trusted')

    await wrapper.get('#welcomeTrustedCidr').setValue('0.0.0.0/0')
    await button(wrapper, 'welcome.network_manual_save').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('welcome.network_manual_too_broad')
    await wrapper.get('#welcomeTrustedCidr').setValue('192.168.7.300/24')
    await button(wrapper, 'welcome.network_manual_save').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('welcome.network_manual_invalid')
    expect(calls().filter(call => call.method === 'PATCH')).toHaveLength(1)

    await wrapper.get('#welcomeTrustedCidr').setValue('172.16.4.20/24')
    await button(wrapper, 'welcome.network_manual_save').trigger('click')
    await settle()
    patches = calls().filter(call => call.method === 'PATCH' && call.url === './api/config')
    expect(patches).toHaveLength(2)
    expect(patches[1].body).toEqual({
      trusted_subnets: '192.168.50.0/24,10.0.0.0/24,172.16.4.0/24',
      trusted_subnet_auto_pairing: 'enabled',
    })
    expect(patches[1].headers['If-Match']).toBe('"r2"')
    expect(wrapper.get('#welcomeTrustedCidr').element.value).toBe('')
    expect(wrapper.text()).not.toContain('welcome.network_manual_invalid')

    // Both saves applied live, so nothing waits for a restart at the end.
    await reachLast(wrapper)
    expect(button(wrapper, 'welcome.restart_now')).toBeFalsy()
    wrapper.unmount()
  })

  it('ends on First App and finishing opens Applications in the same tab', async () => {
    routeFetch(baseRoutes())
    const quiet = vi.spyOn(console, 'error').mockImplementation(() => {})
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 7)

    expect(wrapper.text()).toContain('Step 8 of 8')
    expect(wrapper.get('h2').text()).toBe('First App')
    expect(wrapper.text()).toContain('welcome.first_app_finish')
    expect(button(wrapper, 'Next')).toBeFalsy()
    expect(wrapper.find('a[href="#/apps"]').exists()).toBe(false)

    await button(wrapper, 'Finish Setup').trigger('click')
    expect(window.location.hash).toBe('#/apps')
    wrapper.unmount()
    quiet.mockRestore()
  })

  it('checks a SteamGridDB key before saving it and uses it without a restart', async () => {
    const routes = baseRoutes()
    routes['POST ./api/covers/key/check'] = () => response(200, { status: true, code: 'steamgriddb_ok', matches: 3 })
    routes['PATCH ./api/config'] = () => response(200, { status: true, configuration_revision: 'r2', restart_required: false })
    routeFetch(routes)
    const wrapper = mountWelcome()
    await settle()
    await reachStep(wrapper, 4)
    expect(wrapper.text()).toContain('Step 5 of 8')

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

    await reachLast(wrapper)
    expect(wrapper.text()).toContain('Step 8 of 8')
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

    // The offer is on the last step, First App, not on Pair Client.
    await button(wrapper, 'Next').trigger('click')
    await settle()
    await button(wrapper, 'Next').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('Step 7 of 8')
    expect(wrapper.text()).not.toContain('welcome.restart_needed_one')
    await reachLast(wrapper)
    expect(wrapper.text()).toContain('Step 8 of 8')
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
    expect(wrapper.text()).toContain('Step 6 of 8')
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
    expect(wrapper.text()).toContain('Step 6 of 8')
    await button(wrapper, 'welcome.skip_for_now').trigger('click')
    await settle()
    expect(wrapper.text()).toContain('Step 7 of 8')
    await reachLast(wrapper)
    expect(wrapper.text()).not.toContain('welcome.restart_now')
    expect(calls().some(call => call.method === 'PATCH')).toBe(false)
    wrapper.unmount()
  })
})
