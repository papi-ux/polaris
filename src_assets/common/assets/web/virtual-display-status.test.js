import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { reactive } from 'vue'

import {
  VIRTUAL_DISPLAY_BACKEND_OPTIONS,
  kscreenConnectorOptions,
  presentKscreenConnector,
  presentVirtualDisplayStatus,
} from './virtual-display-status.js'
import VirtualDisplayStatus from './configs/tabs/audiovideo/VirtualDisplayStatus.vue'

describe('virtual display status presentation', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('recovers its status after restart instead of retaining an earlier failure', async () => {
    let ready = false
    vi.stubGlobal('fetch', vi.fn(async (url) => ({
      ok: ready,
      json: async () => String(url).includes('/status')
        ? { available: true, backend_detected: true, backend: 'kscreen-doctor', policy_mode: 'host_virtual_display' }
        : { backends: [] },
    })))
    const config = reactive({ linux_streaming_output: 'DP-2' })
    const wrapper = shallowMount(VirtualDisplayStatus, { props: { platform: 'linux', config, hostGeneration: 0 } })
    await flushPromises()
    expect(wrapper.text()).toContain('Failed to fetch virtual display status')
    ready = true
    await wrapper.setProps({ hostGeneration: 1 })
    await flushPromises()
    expect(wrapper.text()).not.toContain('Failed to fetch virtual display status')
    expect(wrapper.text()).toContain('kscreen-doctor')
    expect(config.linux_streaming_output).toBe('DP-2')
    wrapper.unmount()
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
            // Private Stream does not need a virtual display, but its disabled
            // Host Virtual card must still expose the setup path.
            policy_mode: 'headless_stream',
            unavailable_reason: 'kscreen-doctor backend needs linux_streaming_output set to the output it may reconfigure.',
          }
        : { backends: [] },
    })))
    const config = reactive({ linux_streaming_output: '' })
    const wrapper = shallowMount(VirtualDisplayStatus, {
      props: { platform: 'linux', config },
    })

    await flushPromises()
    expect(wrapper.find('[data-kscreen-configuration]').exists()).toBe(true)
    const connector = wrapper.find('[data-kscreen-streaming-output]')
    expect(connector.exists()).toBe(true)
    await connector.setValue('HDMI-A-2')
    expect(config.linux_streaming_output).toBe('HDMI-A-2')
    expect(wrapper.text()).toContain('separate from the general capture Output Name field')
    wrapper.unmount()
  })
  it('offers exactly the backends the host accepts, Automatic first', () => {
    expect(VIRTUAL_DISPLAY_BACKEND_OPTIONS.map((option) => option.value)).toEqual(['auto', 'evdi', 'kwin', 'wlr', 'kscreen'])
  })

  it('saves the backend choice and explains a KWin screen only when KWin is the backend', async () => {
    vi.stubGlobal('fetch', kscreenFetch({
      status: { available: true, backend: 'KWin virtual output', policy_mode: 'host_virtual_display' },
      outputs: { outputs: [] },
    }))
    const config = reactive({ linux_virtual_display_backend: 'auto', linux_streaming_output: '' })
    const wrapper = shallowMount(VirtualDisplayStatus, { props: { platform: 'linux', config } })

    await flushPromises()
    expect(wrapper.find('[data-kwin-virtual-screen]').exists()).toBe(true)
    expect(wrapper.find('[data-kwin-virtual-screen]').text()).toContain('Nothing is borrowed')
    // The borrowed-connector field belongs to kscreen-doctor alone.
    expect(wrapper.find('[data-kscreen-configuration]').exists()).toBe(false)
    await wrapper.find('[data-vdisplay-backend-select]').setValue('kwin')
    expect(config.linux_virtual_display_backend).toBe('kwin')
    wrapper.unmount()
  })

  it('says why Plasma is not getting a KWin screen when the host reports a reason', async () => {
    vi.stubGlobal('fetch', kscreenFetch({
      status: {
        available: true,
        backend: 'EVDI',
        backend_detected: true,
        kwin_reason: 'kscreen-doctor is not installed; Polaris needs it to place the new screen.',
        policy_mode: 'host_virtual_display',
      },
      outputs: { outputs: [] },
    }))
    const config = reactive({ linux_virtual_display_backend: 'auto', linux_streaming_output: '' })
    const wrapper = shallowMount(VirtualDisplayStatus, { props: { platform: 'linux', config } })

    await flushPromises()
    expect(wrapper.find('[data-kwin-unavailable-reason]').text()).toContain('kscreen-doctor is not installed')
    wrapper.unmount()
  })

  it('keeps the KScreen connector field reachable once the backend is configured', async () => {
    vi.stubGlobal('fetch', kscreenFetch({
      status: { available: true, policy_mode: 'host_virtual_display' },
      outputs: { streaming_output: 'HDMI-A-2', outputs: [
        { name: 'DP-1', connected: true, enabled: true },
        { name: 'HDMI-A-2', connected: true, enabled: false },
      ] },
    }))
    const config = reactive({ linux_streaming_output: 'HDMI-A-2', linux_primary_output: 'DP-1' })
    const wrapper = shallowMount(VirtualDisplayStatus, { props: { platform: 'linux', config } })

    await flushPromises()
    expect(wrapper.find('[data-kscreen-configuration]').exists()).toBe(true)
    expect(wrapper.find('[data-kscreen-connector-state]').text()).toBe('Host Virtual Display will use HDMI-A-2.')
    expect(wrapper.findAll('[data-kscreen-connector-warning]')).toHaveLength(0)
    const options = wrapper.findAll('[data-kscreen-connector-select] option').map((option) => option.text())
    expect(options).toEqual(['Choose a connector', 'HDMI-A-2 (connected, not in use)', 'DP-1 (in use)'])
    wrapper.unmount()
  })

  it('reads the saved connector on a Private Stream host that retired the active one (#633)', async () => {
    vi.stubGlobal('fetch', kscreenFetch({
      status: { available: true, policy_mode: 'headless_stream' },
      outputs: { streaming_output: '', host_virtual_display_output: 'HDMI-A-2', outputs: [
        { name: 'HDMI-A-2', connected: true, enabled: false },
      ] },
    }))
    const config = reactive({ linux_streaming_output: 'HDMI-A-2' })
    const wrapper = shallowMount(VirtualDisplayStatus, { props: { platform: 'linux', config } })

    await flushPromises()
    expect(wrapper.find('[data-kscreen-optional]').exists()).toBe(true)
    expect(wrapper.find('[data-kscreen-connector-state]').text()).toBe('Host Virtual Display will use HDMI-A-2.')
    wrapper.unmount()
  })

  it('says a connector chosen before a restart has not taken effect yet (#633)', async () => {
    vi.stubGlobal('fetch', kscreenFetch({
      status: { available: false, policy_mode: 'headless_stream' },
      outputs: { streaming_output: '', outputs: [{ name: 'DP-1', connected: true, enabled: true }] },
    }))
    const config = reactive({ linux_streaming_output: '', linux_primary_output: 'DP-1' })
    const wrapper = shallowMount(VirtualDisplayStatus, { props: { platform: 'linux', config } })

    await flushPromises()
    expect(wrapper.find('[data-kscreen-optional]').text()).toContain('Private Stream does not use this')
    await wrapper.find('[data-kscreen-streaming-output]').setValue('DP-1')
    expect(wrapper.find('[data-kscreen-connector-state]').text())
      .toBe('DP-1 takes effect after you save and restart Polaris.')
    expect(wrapper.find('[data-kscreen-connector-warning]').text())
      .toContain('looks the same as Mirror Desktop')
    wrapper.unmount()
  })

  it('keeps the last answer when a background refresh lands while the host is still restarting', async () => {
    let hostUp = true
    vi.stubGlobal('fetch', vi.fn(async (url) => {
      if (!hostUp) throw new TypeError('Failed to fetch')
      return kscreenFetch({
        status: { available: true, policy_mode: 'host_virtual_display' },
        outputs: { streaming_output: 'HDMI-A-2', outputs: [{ name: 'HDMI-A-2', connected: true, enabled: false }] },
      })(url)
    }))
    const config = reactive({ linux_streaming_output: 'HDMI-A-2' })
    const wrapper = shallowMount(VirtualDisplayStatus, { props: { platform: 'linux', config, hostGeneration: 0 } })

    await flushPromises()
    expect(wrapper.find('[data-kscreen-connector-state]').text()).toBe('Host Virtual Display will use HDMI-A-2.')
    hostUp = false
    await wrapper.setProps({ hostGeneration: 1 })
    await flushPromises()
    expect(wrapper.text()).not.toContain('Virtual display API not available')
    expect(wrapper.find('[data-kscreen-streaming-output]').exists()).toBe(true)
    expect(wrapper.find('[data-kscreen-connector-state]').text()).toBe('Host Virtual Display will use HDMI-A-2.')
    wrapper.unmount()
  })

  it('never reports an unsaved edit as in use when the connector list is unavailable', async () => {
    vi.stubGlobal('fetch', vi.fn(async (url) => ({
      ok: !String(url).includes('/display-outputs'),
      json: async () => String(url).includes('/vdisplay/status')
        ? { available: true, backend_detected: true, backend: 'kscreen-doctor', policy_mode: 'host_virtual_display' }
        : { backends: [] },
    })))
    const config = reactive({ linux_streaming_output: 'DP-1' })
    const wrapper = shallowMount(VirtualDisplayStatus, { props: { platform: 'linux', config } })

    await flushPromises()
    await wrapper.find('[data-kscreen-streaming-output]').setValue('HDMI-A-9')
    expect(wrapper.find('[data-kscreen-connector-state]').text())
      .toBe('Polaris has a connector loaded. If HDMI-A-9 is a new choice, it takes effect after you save and restart Polaris.')
    expect(wrapper.find('[data-kscreen-connector-select]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('orders spare connectors first and labels what each one is doing', () => {
    expect(kscreenConnectorOptions([
      { name: 'HDMI-A-1', connected: false, enabled: false },
      { name: 'DP-1', connected: true, enabled: true },
      { name: 'DP-2', connected: true, enabled: false },
      { name: '', connected: true, enabled: false },
    ]).map((option) => option.label)).toEqual([
      'DP-2 (connected, not in use)',
      'DP-1 (in use)',
      'HDMI-A-1 (nothing plugged in)',
    ])
    expect(kscreenConnectorOptions(undefined)).toEqual([])
  })

  it('folds a connector name reported by two GPUs into one entry', () => {
    // card0-DP-1 on the iGPU and card1-DP-1 on the discrete card both arrive as DP-1.
    const outputs = [
      { name: 'DP-1', connected: false, enabled: false },
      { name: 'DP-1', connected: true, enabled: false },
    ]
    expect(kscreenConnectorOptions(outputs)).toEqual([
      { name: 'DP-1', connected: true, enabled: false, label: 'DP-1 (connected, not in use)' },
    ])
    expect(presentKscreenConnector({ selected: 'DP-1', loaded: '', outputs }).warnings).toEqual([])
  })

  it('warns about connectors that cannot work as a stream display', () => {
    const outputs = [
      { name: 'DP-1', connected: true, enabled: true },
      { name: 'DP-2', connected: true, enabled: true },
      { name: 'HDMI-A-1', connected: false, enabled: false },
    ]
    const base = { available: false, loaded: '', primary: 'DP-1', outputs }

    expect(presentKscreenConnector({ ...base, selected: '' }).kind).toBe('unset')
    expect(presentKscreenConnector({ ...base, selected: 'DP-1' }).warnings).toEqual([
      'DP-1 is also set as your primary output. Host Virtual Display will take over that monitor for the stream, which looks the same as Mirror Desktop. Pick a spare connector with a dummy plug instead.',
    ])
    expect(presentKscreenConnector({ ...base, selected: 'DP-2' }).warnings).toEqual([
      'DP-2 is on and part of your desktop right now, which is expected for a dummy plug. If it is a monitor you use, pick a spare connector instead, because Host Virtual Display will take it over for the stream.',
    ])
    expect(presentKscreenConnector({ ...base, selected: 'HDMI-A-1' }).warnings).toEqual([
      'Nothing is plugged into HDMI-A-1, so there is no display to turn on and the stream will not start.',
    ])
    expect(presentKscreenConnector({ ...base, selected: 'HDMI-A-9' }).warnings).toEqual([
      'No DRM connector named HDMI-A-9 was found on this host. kscreen-doctor -o lists the names it expects.',
    ])
    // No discovery data: no guesses about names or plugs.
    expect(presentKscreenConnector({ selected: 'HDMI-A-9', outputs: null }).warnings).toEqual([])
  })

  it('names the connector the running host still uses when the choice changed or was cleared', () => {
    expect(presentKscreenConnector({ selected: 'DP-2', loaded: 'DP-1', available: true })).toEqual({
      kind: 'pending',
      message: 'Polaris is still using DP-1. DP-2 takes effect after you save and restart Polaris.',
      warnings: [],
    })
    expect(presentKscreenConnector({ selected: ' DP-2 ', loaded: 'DP-2', available: true }).kind).toBe('ready')
    expect(presentKscreenConnector({ selected: '', loaded: 'DP-1', available: true })).toEqual({
      kind: 'pending',
      message: 'Polaris is still using DP-1. Clearing it takes effect after you save and restart Polaris.',
      warnings: [],
    })
    expect(presentKscreenConnector({ selected: 'DP-2', loaded: null, available: true }).kind).toBe('configured')
  })
})

function kscreenFetch({ status, outputs }) {
  return vi.fn(async (url) => ({
    ok: true,
    json: async () => {
      if (String(url).includes('/vdisplay/status')) {
        return { backend_detected: true, backend: 'kscreen-doctor', ...status }
      }
      if (String(url).includes('/display-outputs')) {
        return { status: true, ...outputs }
      }
      return { backends: [] }
    },
  }))
}
