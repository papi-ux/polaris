import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpacesSetup from './SpacesSetup.vue'
import { dockerAccessCommand, installGuide, validSetup } from '../spaces-setup.js'

const snapshot = (ready = false) => ({
  version: 2, distribution: 'fedora', immutable_host: false, service_uid: 1000,
  host_prerequisites_ready: ready, configured: false, available: false,
  checks: ['docker', 'docker_access', 'identity', 'input', 'gpu', 'security', 'spaces'].map(id => ({
    id, title: id, detail: 'Host check', action: id === 'security' ? 'install_selinux' : '',
    state: id === 'spaces' ? 'not_configured' : ready ? 'ready' : 'required',
  })),
})
const reply = body => ({ ok: true, json: async () => body })
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })

describe('Spaces setup', () => {
  it('offers host commands and rechecks actual state without a system mutation', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot())).mockResolvedValueOnce(reply(snapshot(true))))
    wrapper = mount(SpacesSetup, { global: { stubs: ['router-link', 'SpacesFirstSetup'] } })
    await flushPromises()
    expect(wrapper.text()).toContain('sudo dnf install')
    expect(wrapper.text()).toContain('administrator password stays in that terminal')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('Host prerequisites checked.')
    expect(wrapper.text()).toContain('prepare your first space below')
    expect(fetch).toHaveBeenCalledTimes(2)
    for (const [url, options] of fetch.mock.calls) {
      expect(url).toBe('./api/spaces/setup')
      expect(options.method).toBeUndefined()
    }
  })

  it('clears old successful checks when a refresh fails', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot(true))).mockRejectedValueOnce(new Error('Offline')))
    wrapper = mount(SpacesSetup, { global: { stubs: ['router-link', 'SpacesFirstSetup'] } })
    await flushPromises()
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Offline')
    expect(wrapper.find('[data-setup-check]').exists()).toBe(false)
  })

  it('refuses duplicate, incomplete, or inconsistent setup results', () => {
    expect(validSetup(snapshot())).toBe(true)
    const duplicate = snapshot(); duplicate.checks[1] = duplicate.checks[0]
    const short = snapshot(); short.checks.pop()
    const inconsistent = snapshot(); inconsistent.host_prerequisites_ready = true
    const available = snapshot(); available.available = true
    for (const bad of [null, {}, duplicate, short, inconsistent, available]) expect(validSetup(bad)).toBe(false)
  })

  it('never offers mutable Fedora commands for an immutable host', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ ...snapshot(), immutable_host: true })))
    wrapper = mount(SpacesSetup, { global: { stubs: ['router-link', 'SpacesFirstSetup'] } })
    await flushPromises()
    expect(wrapper.text()).not.toContain('sudo dnf')
    expect(wrapper.text()).toContain('system image')
    expect(wrapper.text()).not.toContain('sudo -H /usr/bin/polaris-spaces-setup')
    expect(installGuide({ distribution: 'constructor', immutable_host: false })).toBeNull()
  })

  it('gates first setup on security readiness and offers only fixed terminal commands', async () => {
    const result = snapshot(true)
    const security = result.checks.find(check => check.id === 'security')
    security.state = 'required'
    result.host_prerequisites_ready = false
    vi.stubGlobal('fetch', vi.fn(async () => reply(result)))
    wrapper = mount(SpacesSetup, { global: { stubs: ['router-link', 'SpacesFirstSetup'] } })
    await flushPromises()
    expect(wrapper.text()).toContain('sudo -H /usr/bin/polaris-spaces-setup install')
    expect(wrapper.text()).toContain('stop Spaces streams, and quit Polaris')
    expect(wrapper.text()).toContain('selinux-policy-devel container-selinux make')
    expect(wrapper.find('spaces-first-setup-stub').attributes('hostready')).toBe('false')
    const old = snapshot(true); old.version = 1
    expect(validSetup(old)).toBe(false)
    security.action = 'sudo untrusted'
    await wrapper.get('button').trigger('click'); await flushPromises()
    expect(wrapper.text()).not.toContain('sudo untrusted')
    expect(wrapper.text()).not.toContain('sudo -H /usr/bin/polaris-spaces-setup')
    expect(fetch).toHaveBeenCalledTimes(2)
  })

  it('uses service account identity, never a browser user or shell payload', () => {
    expect(dockerAccessCommand(1027)).toContain('id -nu -- 1027')
    for (const bad of [0, -1, 3.5, '1000', '$(touch /tmp/unsafe)', 2147483648]) expect(dockerAccessCommand(bad)).toBe('')
  })
})
