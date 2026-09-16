import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpacesSetup from './SpacesSetup.vue'
import { dockerAccessCommand, fedoraSecurityPackages, installGuide, installSpacesSecurity, setupSteps, startDocker, validSetup } from '../spaces-setup.js'
import { spacesGlobal } from './spaces-test-i18n.js'

const snapshot = (ready = false) => ({
  version: 2, distribution: 'fedora', immutable_host: false, service_uid: 1000,
  host_prerequisites_ready: ready, configured: false, available: false,
  checks: ['docker', 'docker_access', 'identity', 'input', 'gpu', 'security', 'spaces'].map(id => ({
    id, title: id, detail: 'Host check', action: id === 'security' ? 'install_selinux' : '',
    state: id === 'spaces' ? 'not_configured' : ready ? 'ready' : 'required',
  })),
})
const reply = body => ({ ok: true, json: async () => body })
const start = () => mount(SpacesSetup, { global: { ...spacesGlobal, stubs: ['router-link', 'SpacesFirstSetup'] } })
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })

describe('Spaces setup', () => {
  it('collapses a configured healthy host and reopens setup when a check fails', async () => {
    const ready = snapshot(true)
    ready.configured = true; ready.available = true
    ready.checks.find(check => check.id === 'spaces').state = 'ready'
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(ready)).mockRejectedValueOnce(new Error('Offline')))
    wrapper = start()
    await flushPromises()
    expect(wrapper.element.open).toBe(false)
    expect(wrapper.emitted('state').at(-1)).toEqual([{ available: true, configured: true, hostReady: true }])
    await wrapper.get('button').trigger('click'); await flushPromises()
    expect(wrapper.element.open).toBe(true)
    expect(wrapper.get('summary').text()).toContain('Needs attention')
  })

  it('does not collapse under the reader when a recheck comes back healthy', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot())).mockResolvedValueOnce(reply(snapshot(true))))
    wrapper = start()
    await flushPromises()
    expect(wrapper.element.open).toBe(true)
    const checks = wrapper.findAll('details').at(1)
    expect(checks.element.open).toBe(true)
    await wrapper.get('button').trigger('click'); await flushPromises()
    expect(wrapper.element.open).toBe(true)
    expect(wrapper.findAll('details').at(1).element.open).toBe(true)
    expect(wrapper.get('.control-chip').text()).toBe('6/7')
  })

  it('links each failing check to its guide section and shows the terminal steps for a mutable host', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot())).mockResolvedValueOnce(reply(snapshot(true))))
    wrapper = start()
    await flushPromises()
    expect(wrapper.get('[data-setup-check=docker] a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#prepare-docker-from-spaces')
    expect(wrapper.get('[data-setup-check=identity] a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#gaming-runtime-account')
    const docker = wrapper.get('[data-setup-check=docker] [data-setup-steps]').text()
    expect(docker).toContain('sudo dnf config-manager addrepo')
    expect(docker).toContain(startDocker)
    expect(wrapper.get('[data-setup-check=docker_access] [data-setup-steps]').text()).toContain(dockerAccessCommand(1000))
    expect(wrapper.find('[data-setup-check=identity] [data-setup-steps]').exists()).toBe(false)
    expect(wrapper.find('pre').exists()).toBe(false)
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('Host prerequisites checked.')
    expect(wrapper.text()).toContain('prepare your first Space below')
    expect(wrapper.find('[data-setup-steps]').exists()).toBe(false)
    expect(fetch).toHaveBeenCalledTimes(2)
    for (const [url, options] of fetch.mock.calls) {
      expect(url).toBe('./api/spaces/setup')
      expect(options.method).toBeUndefined()
    }
  })

  it('prefers the section the host names over the historical one', async () => {
    const result = snapshot()
    result.checks.find(check => check.id === 'security').doc_anchor = '#security-support'
    vi.stubGlobal('fetch', vi.fn(async () => reply(result)))
    wrapper = start()
    await flushPromises()
    expect(wrapper.get('[data-setup-check=security] a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#security-support')
    const bad = snapshot(); bad.checks[0].doc_anchor = 'javascript:alert(1)'
    expect(validSetup(bad)).toBe(false)
  })

  it('copies a step command and says so', async () => {
    const writeText = vi.fn(async () => {})
    vi.stubGlobal('navigator', { clipboard: { writeText } })
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    wrapper = start()
    await flushPromises()
    const copy = wrapper.get('[data-setup-check=docker_access] [data-setup-steps] button')
    await copy.trigger('click')
    await flushPromises()
    expect(writeText).toHaveBeenCalledWith(startDocker)
    expect(copy.text()).toBe('Copied')
  })

  it('clears old successful checks when a refresh fails', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot(true))).mockRejectedValueOnce(new Error('Offline')))
    wrapper = start()
    await flushPromises()
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Offline')
    expect(wrapper.find('[data-setup-check]').exists()).toBe(false)
    expect(wrapper.emitted('state').at(-1)).toEqual([null])
  })

  it('refuses duplicate, incomplete, or inconsistent setup results', () => {
    expect(validSetup(snapshot())).toBe(true)
    const duplicate = snapshot(); duplicate.checks[1] = duplicate.checks[0]
    const short = snapshot(); short.checks.pop()
    const inconsistent = snapshot(); inconsistent.host_prerequisites_ready = true
    const available = snapshot(); available.available = true
    for (const bad of [null, {}, duplicate, short, inconsistent, available]) expect(validSetup(bad)).toBe(false)
  })

  it('never offers package commands for an immutable host', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ ...snapshot(), immutable_host: true })))
    wrapper = start()
    await flushPromises()
    expect(wrapper.text()).not.toContain('sudo dnf')
    expect(wrapper.get('[data-setup-check=docker] a').text()).toBe('Docker setup guide')
    expect(wrapper.text()).not.toContain('sudo -H /usr/bin/polaris-spaces-setup')
    expect(wrapper.get('[data-setup-check=docker] [data-setup-steps]').text()).toContain(startDocker)
    expect(installGuide({ distribution: 'constructor', immutable_host: false })).toBeNull()
    expect(setupSteps({ ...snapshot(), immutable_host: true }, { id: 'security', state: 'required' })).toEqual([])
  })

  it('gates first setup on security readiness and shows only the fixed helper command', async () => {
    const result = snapshot(true)
    const security = result.checks.find(check => check.id === 'security')
    security.state = 'required'
    result.host_prerequisites_ready = false
    vi.stubGlobal('fetch', vi.fn(async () => reply(result)))
    wrapper = start()
    await flushPromises()
    expect(wrapper.get('[data-setup-check=security] a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#prepare-spaces-security-support')
    expect(wrapper.find('pre').exists()).toBe(false)
    expect(wrapper.find('spaces-first-setup-stub').attributes('hostready')).toBe('false')
    const steps = wrapper.get('[data-setup-check=security] [data-setup-steps]').text()
    expect(steps).toContain(fedoraSecurityPackages)
    expect(steps).toContain(installSpacesSecurity)
    const old = snapshot(true); old.version = 1
    expect(validSetup(old)).toBe(false)
    security.action = 'sudo untrusted'
    await wrapper.get('button').trigger('click'); await flushPromises()
    expect(wrapper.text()).not.toContain('sudo untrusted')
    expect(fetch).toHaveBeenCalledTimes(2)
  })

  it('uses service account identity, never a browser user or shell payload', () => {
    expect(dockerAccessCommand(1027)).toContain('id -nu -- 1027')
    for (const bad of [0, -1, 3.5, '1000', '$(touch /tmp/unsafe)', 2147483648]) expect(dockerAccessCommand(bad)).toBe('')
  })
})
