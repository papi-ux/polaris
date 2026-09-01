import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { reactive } from 'vue'

import { presentVirtualDisplayStatus } from './virtual-display-status.js'
import VirtualDisplayStatus from './configs/tabs/audiovideo/VirtualDisplayStatus.vue'

describe('virtual display status presentation', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('explains that Private Stream intentionally bypasses host virtual displays', () => {
    const state = presentVirtualDisplayStatus({
      policy_mode: 'headless_stream',
      available: false,
      backend_detected: false,
    })

    expect(state.kind).toBe('unused')
    expect(state.label).toBe('Not needed for Private Stream')
  })

  it('shows backend-specific configuration guidance when kscreen-doctor is detected', () => {
    const state = presentVirtualDisplayStatus({
      policy_mode: 'host_virtual_display',
      available: false,
      backend_detected: true,
      backend: 'kscreen-doctor',
      unavailable_reason: 'kscreen-doctor backend needs linux_streaming_output set to the output it may reconfigure.',
    })

    expect(state.kind).toBe('unconfigured')
    expect(state.detail).toContain('linux_streaming_output')
  })

  it('reserves dependency guidance for a genuinely missing backend', () => {
    expect(presentVirtualDisplayStatus({
      policy_mode: 'host_virtual_display',
      available: false,
      backend_detected: false,
    }).kind).toBe('missing')
  })

  it('lets an installed KScreen backend be configured from its readiness guidance', async () => {
    vi.stubGlobal('fetch', vi.fn(async (url) => ({
      ok: true,
      json: async () => String(url).includes('/status')
        ? {
            available: false,
            backend_detected: true,
            backend: 'kscreen-doctor',
            policy_mode: 'host_virtual_display',
            unavailable_reason: 'kscreen-doctor backend needs linux_streaming_output set to the output it may reconfigure.',
          }
        : { backends: [] },
    })))
    const config = reactive({ linux_streaming_output: '' })
    const wrapper = shallowMount(VirtualDisplayStatus, {
      props: { platform: 'linux', config },
    })

    await flushPromises()
    const connector = wrapper.find('[data-kscreen-streaming-output]')
    expect(connector.exists()).toBe(true)
    await connector.setValue('HDMI-A-2')
    expect(config.linux_streaming_output).toBe('HDMI-A-2')
    expect(wrapper.text()).toContain('separate from the general capture Output Name field')
  })
})
