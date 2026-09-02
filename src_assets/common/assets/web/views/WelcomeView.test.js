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
