import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import SpacesSetup from './SpacesSetup.vue'
import SpacesFirstSetup from './SpacesFirstSetup.vue'
import { dockerAccessCommand, fedoraSecurityPackages, installGuide, installSpacesSecurity, setupSteps, startDocker, validSetup } from '../spaces-setup.js'
import { spacesGlobal } from './spaces-test-i18n.js'
import { validHostActionSnapshot } from '../spaces-job.js'

const runtimeCodes = { not_published: 'runtime_not_published', unsupported: 'driver_mismatch', available: 'not_downloaded', ready: 'runtime_ready', failed: 'inspection_failed' }
const runtimeDetails = {
  not_published: 'This Polaris build has no approved gaming runtime yet.',
  unsupported: 'The gaming runtime in this Polaris build needs NVIDIA driver 610.57.04. This PC runs 580.95.05. Install the matching driver, restart the PC, then recheck.',
  available: 'This PC needs the gaming runtime for NVIDIA driver 610.57.04. It is not downloaded yet, and the download is several gigabytes.',
  ready: 'The gaming runtime for NVIDIA driver 610.57.04 is downloaded and verified.',
  failed: 'Polaris could not verify the gaming runtime on this PC. Retry, and check Docker if it keeps failing.',
}
// The gaming runtime check as the host reports it for each status.
const runtimeCheck = status => ({
  id: 'runtime', title: 'Gaming runtime', detail: runtimeDetails[status], doc_anchor: '#download-the-gaming-runtime',
  action: status === 'available' ? 'download_runtime' : status === 'failed' ? 'retry_runtime' : '',
  state: status === 'ready' ? 'ready' : status === 'not_published' ? 'not_configured' : 'required',
  runtime: { status, code: runtimeCodes[status],
    ...(['available', 'ready', 'failed'].includes(status) ? { id: 'steam-nvidia-610', variant: 'nvidia', nvidia_driver: '610.57.04' } : {}) },
})
// runtime: the runtime check's status, or null for a host from before that check.
const snapshot = (ready = false, runtime = 'available') => ({
  version: 2, distribution: 'fedora', immutable_host: false, service_uid: 1000,
  host_prerequisites_ready: ready, configured: false, available: false,
  checks: [
    ...['docker', 'docker_access', 'identity', 'input', 'gpu', 'security'].map(id => ({
      id, title: id, detail: 'Host check', action: id === 'security' ? 'install_selinux' : '', state: ready ? 'ready' : 'required',
    })),
    ...(runtime ? [runtimeCheck(runtime)] : []),
    { id: 'spaces', title: 'spaces', detail: 'Host check', action: '', state: 'not_configured' },
  ],
})
const connection = (extra = {}) => ({ available: true, reason: '', download: null, job: '', ...extra })
const running = (phase = 'downloading') => ({
  request_id: '22345678-1234-1234-1234-123456789abc', runtime_id: 'steam-nvidia-610', state: phase, code: phase,
  message: phase === 'failed' ? 'The download did not finish. Retry to reuse verified layers.' : 'Checking and downloading the gaming runtime.',
  can_cancel: phase === 'downloading',
})
const reply = body => ({ ok: true, json: async () => body })
const start = () => mount(SpacesSetup, { global: { ...spacesGlobal, stubs: ['router-link', 'SpacesFirstSetup'] } })
// First-Space setup carries the download; this stands in for its connection.
const firstSetup = { name: 'SpacesFirstSetup', props: ['hostReady', 'readyRuntimeId'], emits: ['runtime'], template: '<div data-first-setup />',
  methods: { download: vi.fn(async () => ''), stopDownload: vi.fn(async () => '') } }
const startWithJob = () => mount(SpacesSetup, { global: { ...spacesGlobal, stubs: { 'router-link': true, SpacesFirstSetup: firstSetup } } })
const report = extra => wrapper.findComponent(firstSetup).vm.$emit('runtime', connection(extra))
const row = id => wrapper.get(`[data-setup-check=${id}]`)
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.clearAllMocks() })

describe('Spaces setup', () => {
  it('collapses a configured healthy host and reopens setup when a check fails', async () => {
    const ready = snapshot(true, 'ready')
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
    expect(wrapper.get('.control-chip').text()).toBe('6/8')
  })

  it('waits on the runtime check alone when the build has no runtime', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot(true, 'not_published'))))
    wrapper = start()
    await flushPromises()
    const runtime = row('runtime')
    expect(runtime.get('[data-check-waiting]').text()).toBe('Waiting for runtime')
    expect(runtime.text()).toContain('This Polaris build has no approved gaming runtime yet.')
    expect(runtime.text()).not.toContain('Needs attention')
    expect(runtime.find('a').exists()).toBe(false)
    expect(runtime.find('[data-runtime-action]').exists()).toBe(false)
    // One waiting row: Spaces configuration is not repeated underneath it.
    expect(wrapper.find('[data-setup-check=spaces]').exists()).toBe(false)
    expect(wrapper.findAll('[data-check-waiting]')).toHaveLength(1)
    expect(wrapper.get('[data-setup-count]').text()).toBe('6/6')
    const status = wrapper.get('[role=status]').text()
    expect(status).toContain('Host prerequisites checked.')
    expect(status).toContain('Spaces wait for a Polaris build that includes a verified gaming runtime.')
    expect(status).not.toContain('still needs attention')
    expect(wrapper.get('summary').text()).toContain('Host ready')
    expect(wrapper.findAll('details').at(1).element.open).toBe(false)
    expect(wrapper.emitted('runtime-waiting').at(-1)).toEqual([true])
    // First-Space setup saying the same thing adds nothing.
    wrapper.findComponent(SpacesFirstSetup).vm.$emit('runtime', connection({ available: false, reason: 'runtime_not_published' }))
    await flushPromises()
    expect(wrapper.findAll('[data-check-waiting]')).toHaveLength(1)
    expect(wrapper.get('[data-setup-count]').text()).toBe('6/6')
  })

  it('reads a host from before the runtime check the way it always did', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot(true, null))))
    wrapper = start()
    await flushPromises()
    expect(wrapper.get('.control-chip').text()).toBe('6/7')
    expect(wrapper.find('[data-setup-check=runtime]').exists()).toBe(false)
    wrapper.findComponent(SpacesFirstSetup).vm.$emit('runtime', { available: false, reason: 'runtime_not_published' })
    await flushPromises()
    const spaces = row('spaces')
    expect(spaces.get('[data-check-waiting]').text()).toBe('Waiting for runtime')
    expect(spaces.text()).toContain('This Polaris build has no verified gaming runtime yet, so there is nothing to configure here.')
    expect(spaces.text()).not.toContain('Not configured')
    expect(spaces.find('a').exists()).toBe(false)
    expect(wrapper.get('.control-chip').text()).toBe('6/6')
    expect(wrapper.get('summary').text()).toContain('Host ready')
    expect(wrapper.emitted('runtime-waiting').at(-1)).toEqual([true])
  })

  it('keeps asking for the first Space when the build offers a runtime', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot(true))))
    wrapper = start()
    await flushPromises()
    wrapper.findComponent(SpacesFirstSetup).vm.$emit('runtime', connection())
    await flushPromises()
    const spaces = row('spaces')
    expect(spaces.find('[data-check-waiting]').exists()).toBe(false)
    expect(spaces.text()).toContain('Not configured')
    expect(spaces.get('a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#prepare-your-first-space')
    expect(spaces.text()).toContain('prepare your first Space below')
    expect(spaces.text()).not.toContain('The preview shows whether')
    expect(wrapper.get('.control-chip').text()).toBe('6/8')
    expect(wrapper.get('[role=status]').text()).toContain('Spaces configuration still needs attention.')
    expect(wrapper.get('summary').text()).toContain('Set up Spaces')
    expect(wrapper.emitted('runtime-waiting')).toBeUndefined()
  })

  it('still reads a failing host check as a failure while the build has no runtime', async () => {
    const result = snapshot(true, 'not_published')
    result.checks.find(check => check.id === 'security').state = 'required'
    result.host_prerequisites_ready = false
    vi.stubGlobal('fetch', vi.fn(async () => reply(result)))
    wrapper = start()
    await flushPromises()
    const security = row('security')
    expect(security.text()).toContain('Needs attention')
    expect(security.get('a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#prepare-spaces-security-support')
    expect(wrapper.get('[data-setup-check=runtime] [data-check-waiting]').text()).toBe('Waiting for runtime')
    expect(wrapper.get('.control-chip').text()).toBe('5/6')
    const status = wrapper.get('[role=status]').text()
    expect(status).toContain('Complete the steps below, then recheck setup.')
    expect(status).toContain('Spaces wait for a Polaris build')
    expect(wrapper.get('summary').text()).toContain('Set up Spaces')
    expect(wrapper.get('summary').text()).not.toContain('Host ready')
    expect(wrapper.findAll('details').at(1).element.open).toBe(true)
  })

  it('offers the download on the runtime check, shows its progress and counts it once verified', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot(true))).mockResolvedValueOnce(reply(snapshot(true, 'ready'))))
    wrapper = startWithJob()
    await flushPromises()
    // A runtime to download opens the checks even though the host is ready.
    expect(wrapper.findAll('details').at(1).element.open).toBe(true)
    expect(wrapper.get('[data-setup-count]').text()).toBe('6/8')
    // Nothing is verified yet, so first Space setup still downloads.
    expect(wrapper.findComponent(firstSetup).props('readyRuntimeId')).toBe('')
    const available = row('runtime')
    expect(available.text()).toContain('Not downloaded')
    expect(available.text()).not.toContain('Needs attention')
    expect(available.text()).toContain('It is not downloaded yet, and the download is several gigabytes.')
    expect(available.get('a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#download-the-gaming-runtime')
    // No button until the job connection answers.
    expect(available.find('[data-runtime-download]').exists()).toBe(false)
    report()
    await flushPromises()
    const download = row('runtime').get('[data-runtime-download]')
    expect(download.text()).toBe('Download')
    expect(download.attributes('aria-label')).toBe('Download: Gaming runtime')
    expect(download.attributes('disabled')).toBeUndefined()
    await download.trigger('click')
    await flushPromises()
    expect(firstSetup.methods.download).toHaveBeenCalledTimes(1)
    expect(firstSetup.methods.download).toHaveBeenCalledWith('steam-nvidia-610')

    report({ download: running() })
    await flushPromises()
    const downloading = row('runtime')
    expect(downloading.text()).toContain('Downloading')
    expect(downloading.get('[role=status]').text()).toBe('Downloading the gaming runtime. You can leave this page and return later.')
    expect(downloading.find('[data-runtime-download]').exists()).toBe(false)
    await downloading.get('[data-runtime-stop]').trigger('click')
    await flushPromises()
    expect(firstSetup.methods.stopDownload).toHaveBeenCalledTimes(1)
    expect(fetch).toHaveBeenCalledTimes(1)

    // The finished download is read back from the host, not assumed.
    report({ download: { ...running('ready'), code: 'runtime_ready', message: 'The gaming runtime is downloaded and verified.' } })
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(2)
    const ready = row('runtime')
    expect(ready.text()).toContain('Checked')
    expect(ready.text()).toContain('downloaded and verified')
    expect(ready.find('[data-runtime-action]').exists()).toBe(false)
    expect(ready.find('a').exists()).toBe(false)
    expect(wrapper.get('[data-setup-count]').text()).toBe('7/8')
    expect(wrapper.findComponent(firstSetup).props('readyRuntimeId')).toBe('steam-nvidia-610')
  })

  it('also shows progress while first-Space setup downloads the runtime, without a second download', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot(true))).mockResolvedValueOnce(reply(snapshot(true, 'ready'))))
    wrapper = startWithJob()
    await flushPromises()
    report({ job: 'downloading' })
    await flushPromises()
    expect(row('runtime').text()).toContain('Downloading')
    expect(row('runtime').find('[data-runtime-download]').exists()).toBe(false)
    expect(row('runtime').find('[data-runtime-stop]').exists()).toBe(false)
    report({ job: 'preparing' })
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(2)
    expect(row('runtime').text()).toContain('Checked')
  })

  it('names a failed runtime check, retries the download there and keeps its errors on the check', async () => {
    firstSetup.methods.download.mockResolvedValueOnce('This request was not accepted. Check the saved setup shown here before retrying.')
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot(true, 'failed'))))
    wrapper = startWithJob()
    await flushPromises()
    report({ download: running('failed') })
    await flushPromises()
    const failed = row('runtime')
    expect(failed.text()).toContain('Needs attention')
    expect(failed.text()).toContain('Polaris could not verify the gaming runtime on this PC.')
    expect(failed.get('[data-runtime-outcome]').text()).toBe('The download did not finish. Retry to reuse verified layers.')
    const retry = failed.get('[data-runtime-download]')
    expect(retry.text()).toBe('Retry download')
    await retry.trigger('click')
    await flushPromises()
    expect(firstSetup.methods.download).toHaveBeenCalledWith('steam-nvidia-610')
    expect(row('runtime').get('[role=alert]').text()).toBe('This request was not accepted. Check the saved setup shown here before retrying.')
  })

  it('waits for the host checks and a job connection before offering a download', async () => {
    const result = snapshot(true)
    result.checks.find(check => check.id === 'input').state = 'required'
    result.host_prerequisites_ready = false
    vi.stubGlobal('fetch', vi.fn(async () => reply(result)))
    wrapper = startWithJob()
    await flushPromises()
    report()
    await flushPromises()
    const blocked = row('runtime')
    expect(blocked.get('[data-runtime-download]').attributes('disabled')).toBeDefined()
    expect(blocked.text()).toContain('Complete the host checks above before downloading.')
    await blocked.get('[data-runtime-download]').trigger('click')
    expect(firstSetup.methods.download).not.toHaveBeenCalled()
    // A setup job the host cannot run offers no download at all.
    report({ available: false, reason: 'journal_locked' })
    await flushPromises()
    expect(row('runtime').find('[data-runtime-download]').exists()).toBe(false)
  })

  it('offers no download on a configured host or for a runtime this PC cannot use', async () => {
    const configured = snapshot(true)
    configured.configured = true
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(configured)).mockResolvedValueOnce(reply(snapshot(true, 'unsupported'))))
    wrapper = startWithJob()
    await flushPromises()
    expect(wrapper.find('[data-first-setup]').exists()).toBe(false)
    expect(row('runtime').find('[data-runtime-download]').exists()).toBe(false)
    await wrapper.get('[data-spaces-recheck]').trigger('click')
    await flushPromises()
    report()
    await flushPromises()
    const unsupported = row('runtime')
    expect(unsupported.text()).toContain('Needs attention')
    expect(unsupported.text()).toContain('Install the matching driver, restart the PC, then recheck.')
    expect(unsupported.find('[data-runtime-action]').exists()).toBe(false)
    expect(unsupported.get('a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#download-the-gaming-runtime')
    expect(wrapper.get('[data-setup-count]').text()).toBe('6/8')
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

  it('accepts the NVIDIA driver files check, and still rejects an unknown one', async () => {
    // A host with an NVIDIA driver loaded sends one more check than a host
    // without one. Counting rows instead of naming them made the whole
    // response fail to verify, which reads as "recheck setup" forever.
    const nvidia = snapshot(true)
    nvidia.checks.splice(6, 0, {
      id: 'nvidia_libraries', title: 'NVIDIA driver files', doc_anchor: '#nvidia-driver-files',
      detail: "This PC's NVIDIA driver files are ready for a space to borrow.", action: '', state: 'ready',
    })
    expect(validSetup(nvidia)).toBe(true)

    // It does not decide whether the host is ready, so a host that is missing
    // the 32 bit half still sets up.
    const missing = snapshot(true)
    missing.checks.splice(6, 0, {
      id: 'nvidia_libraries', title: 'NVIDIA driver files', doc_anchor: '#nvidia-driver-files',
      detail: 'Install xorg-x11-drv-nvidia-libs.i686.', action: '', state: 'required',
    })
    expect(validSetup(missing)).toBe(true)
    expect(missing.host_prerequisites_ready).toBe(true)

    // A host without an NVIDIA driver sends no such row at all.
    expect(validSetup(snapshot(true))).toBe(true)

    const unknown = snapshot(true)
    unknown.checks.push({ id: 'something_else', title: 'x', detail: 'x', action: '', state: 'ready' })
    expect(validSetup(unknown)).toBe(false)

    const incomplete = snapshot(true)
    incomplete.checks = incomplete.checks.filter(check => check.id !== 'gpu')
    expect(validSetup(incomplete)).toBe(false)
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
    for (const status of ['not_published', 'unsupported', 'available', 'ready', 'failed']) expect(validSetup(snapshot(true, status))).toBe(true)
    expect(validSetup(snapshot(true, null))).toBe(true)
    const duplicate = snapshot(); duplicate.checks[1] = duplicate.checks[0]
    const short = snapshot(); short.checks.pop()
    const inconsistent = snapshot(); inconsistent.host_prerequisites_ready = true
    const available = snapshot(); available.available = true
    const runtimeState = snapshot(true, 'available'); runtimeState.checks[6].state = 'ready'
    const neutralFailure = snapshot(true, 'failed'); neutralFailure.checks[6].state = 'not_configured'
    const unnamed = snapshot(true, 'ready'); delete unnamed.checks[6].runtime.id
    const named = snapshot(true, 'not_published'); named.checks[6].runtime.id = 'steam-nvidia-610'
    const driver = snapshot(true, 'available'); driver.checks[6].runtime.nvidia_driver = '610; rm'
    const status = snapshot(true, 'available'); status.checks[6].runtime.status = 'downloaded'
    const hostNeutral = snapshot(true); hostNeutral.checks[0].state = 'not_configured'
    for (const bad of [null, {}, duplicate, short, inconsistent, available, runtimeState, neutralFailure, unnamed, named, driver, status, hostNeutral]) {
      expect(validSetup(bad)).toBe(false)
    }
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

describe('Spaces host setup approved at the PC', () => {
  const requestId = '32345678-1234-4234-8234-123456789abc'
  const working = ['waiting_for_approval', 'running']
  const hostReply = (body, status = 200) => ({ ok: status >= 200 && status < 300, status, json: async () => body })
  const idle = (extra = {}) => ({ version: 1, available: true, job: null, ...extra })
  const job = (state, action = 'security_install', extra = {}, id = requestId) => ({
    version: 1, available: !working.includes(state),
    ...(working.includes(state) ? { reason: 'host_setup_running', message: "Polaris is already waiting on a change to this PC's setup. Wait for it to finish." } : {}),
    job: { request_id: id, action, state, message: 'Words from the host', detail: '', ...extra },
  })
  const failing = (id, hostAction) => {
    const result = snapshot(false)
    result.checks.find(item => item.id === id).host_action = hostAction
    return result
  }
  // Each endpoint answers from its own queue; the last answer repeats.
  function host({ setup, action }) {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/spaces/setup') return reply(setup.length > 1 ? setup.shift() : setup[0])
      if (url === './api/spaces/setup/host-action') return action.length > 1 ? action.shift() : action[0]
      throw new Error('unexpected ' + url)
    }))
  }
  const calls = url => fetch.mock.calls.filter(([called]) => called === url)
  beforeEach(() => { vi.stubGlobal('crypto', { randomUUID: () => requestId }) })
  afterEach(() => { vi.useRealTimers() })

  it('offers the fix on the check it fixes, follows the prompt and rechecks when the change ends', async () => {
    vi.useFakeTimers()
    const fixed = snapshot(false)
    host({
      setup: [failing('security', 'security_install'), fixed],
      action: [hostReply(idle()), hostReply({ ...job('waiting_for_approval'), accepted: true }, 202), hostReply(job('running')),
        hostReply(job('done', 'security_install', { check: { id: 'security', state: 'ready' } }))],
    })
    fixed.checks.find(item => item.id === 'security').state = 'ready'
    wrapper = start()
    await vi.advanceTimersByTimeAsync(0)
    const button = row('security').get('[data-host-action-start]')
    expect(button.text()).toBe('Install security support')
    expect(button.attributes('aria-label')).toBe('Install security support: security')
    expect(row('security').text()).toContain("A password prompt opens on this PC's screen, and someone at this PC has to approve it.")
    expect(row('docker_access').find('[data-host-action]').exists()).toBe(false)
    // The terminal steps stay beside the button.
    expect(row('security').find('[data-setup-steps]').exists()).toBe(true)

    await button.trigger('click')
    await vi.advanceTimersByTimeAsync(0)
    const post = calls('./api/spaces/setup/host-action').find(([, options]) => options.method === 'POST')
    expect(JSON.parse(post[1].body)).toEqual({ action: 'security_install', request_id: requestId })
    expect(row('security').get('[data-host-action-state]').text()).toBe("Waiting for approval on the PC's screen.")
    expect(row('security').get('[data-host-action-state]').attributes('role')).toBe('status')
    expect(row('security').find('[data-host-action-start]').exists()).toBe(false)

    await vi.advanceTimersByTimeAsync(2000)
    expect(row('security').get('[data-host-action-state]').text()).toBe("Approved. Polaris is changing this PC's setup.")
    expect(calls('./api/spaces/setup')).toHaveLength(1)

    await vi.advanceTimersByTimeAsync(2000)
    expect(calls('./api/spaces/setup')).toHaveLength(2)
    expect(row('security').get('[data-host-action-outcome]').text()).toBe('Spaces security support is installed.')
    expect(row('security').find('[data-host-action-start]').exists()).toBe(false)
    const asked = calls('./api/spaces/setup/host-action').length
    await vi.advanceTimersByTimeAsync(10000)
    expect(calls('./api/spaces/setup/host-action')).toHaveLength(asked)
  })

  it('says why the fix cannot start, in the console words or the host sentence', async () => {
    host({ setup: [failing('docker_access', 'docker_access')],
      action: [hostReply(idle({ available: false, reason: 'no_local_desktop', message: 'Host sentence.' }))] })
    wrapper = start()
    await flushPromises()
    expect(row('docker_access').get('[data-host-action-start]').text()).toBe('Give Polaris access to Docker')
    expect(row('docker_access').get('[data-host-action-start]').attributes('disabled')).toBeDefined()
    expect(row('docker_access').get('[data-host-action-unavailable]').text())
      .toBe("Someone must be signed in at this PC's desktop to approve the password prompt. Otherwise, use the terminal steps below.")
    expect(row('docker_access').text()).not.toContain('A password prompt opens')
    wrapper.unmount()
    host({ setup: [failing('docker_access', 'docker_access')],
      action: [hostReply(idle({ available: false, reason: 'a_later_reason', message: 'A sentence from a newer host.' }))] })
    wrapper = start()
    await flushPromises()
    expect(row('docker_access').get('[data-host-action-unavailable]').text()).toBe('A sentence from a newer host.')
  })

  it('shows a refusal at the moment of asking and keeps the button', async () => {
    const refusal = { code: 'stream_active', message: 'End every stream on this PC, then try again.' }
    host({ setup: [failing('security', 'security_install')],
      action: [hostReply(idle()), hostReply({ ...idle({ available: false, reason: 'stream_active', message: refusal.message }), accepted: false, refusal }, 409)] })
    wrapper = start()
    await flushPromises()
    await row('security').get('[data-host-action-start]').trigger('click')
    await flushPromises()
    expect(row('security').get('[role=alert]').text()).toBe('End every stream on this PC first.')
    expect(row('security').find('[data-host-action-state]').exists()).toBe(false)
    expect(row('security').find('[data-host-action-start]').exists()).toBe(true)
  })

  it('keeps the helper words when it refuses, and offers the button again', async () => {
    vi.useFakeTimers()
    const words = 'Stop Spaces streams and quit any other Polaris before changing security setup. Still active: polaris-spaces-ux (pid 42).'
    host({ setup: [failing('security', 'security_install')],
      action: [hostReply(idle()), hostReply({ ...job('waiting_for_approval'), accepted: true }, 202), hostReply(job('refused', 'security_install', { detail: words }))] })
    wrapper = start()
    await vi.advanceTimersByTimeAsync(0)
    await row('security').get('[data-host-action-start]').trigger('click')
    await vi.advanceTimersByTimeAsync(2000)
    expect(row('security').get('[data-host-action-outcome]').text()).toBe('The Spaces setup helper did not make the change.')
    expect(row('security').get('[data-host-action-detail]').text()).toBe(words)
    expect(row('security').get('[data-host-action-start]').attributes('disabled')).toBeUndefined()
  })

  it('stays quiet about an outcome from before the page opened, and shows a prompt already open', async () => {
    host({ setup: [failing('security', 'security_install')], action: [hostReply(job('cancelled'))] })
    wrapper = start()
    await flushPromises()
    expect(row('security').find('[data-host-action-outcome]').exists()).toBe(false)
    expect(row('security').find('[data-host-action-start]').exists()).toBe(true)
    wrapper.unmount()
    host({ setup: [failing('security', 'security_install')], action: [hostReply(job('waiting_for_approval'))] })
    wrapper = start()
    await flushPromises()
    expect(row('security').get('[data-host-action-state]').text()).toBe("Waiting for approval on the PC's screen.")
  })

  it('offers no button on a host without in-app setup', async () => {
    host({ setup: [failing('security', 'security_install')], action: [hostReply({}, 404)] })
    wrapper = start()
    await flushPromises()
    expect(wrapper.find('[data-host-action]').exists()).toBe(false)
    expect(row('security').find('[data-setup-steps]').exists()).toBe(true)
  })

  it('reads only host action snapshots it understands', () => {
    expect(validHostActionSnapshot(idle())).toBe(true)
    expect(validHostActionSnapshot(job('waiting_for_approval'))).toBe(true)
    expect(validHostActionSnapshot(job('done', 'docker_access', { check: { id: 'docker_access', state: 'ready' } }))).toBe(true)
    const done = job('done').job
    for (const bad of [
      { ...idle(), version: 2 },
      idle({ reason: 'stream_active' }),
      { version: 1, available: false, job: null },
      idle({ job: { ...done, action: 'docker_install' } }),
      idle({ job: { ...done, state: 'approved' } }),
      idle({ job: { ...done, request_id: 'not-a-uuid' } }),
      idle({ job: { ...done, detail: 'x'.repeat(4097) } }),
      idle({ refusal: { code: 'Bad Code', message: 'x' } }),
    ]) expect(validHostActionSnapshot(bad)).toBe(false)
  })
})
