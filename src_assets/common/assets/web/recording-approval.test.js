import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'

import RecordingApprovalToggle from './components/RecordingApprovalToggle.vue'
import {
  permissionMapping,
  permissionPresetKey,
  permissionPresetMask,
  recordingAuthorizationUpdate,
  useClients,
} from './composables/useClients'

// The real English strings, so a missing locale key fails here instead of rendering a raw key.
const en = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const t = (key) => {
  const value = key.split('.').reduce((node, part) => node?.[part], en)
  return typeof value === 'string' ? value : key
}
const web = (path) => readFileSync(resolve(process.cwd(), 'src_assets/common/assets/web', path), 'utf8')

// The parent owns the model, as PinView does: it applies each update after the event.
function mountToggle(props) {
  let wrapper = null
  wrapper = mount(RecordingApprovalToggle, {
    props: {
      idSuffix: 'device-1',
      'onUpdate:approved': (value) => Promise.resolve().then(() => wrapper.setProps({ approved: value })),
      ...props,
    },
    global: { mocks: { $t: t } },
  })
  return wrapper
}

function editedClient(overrides = {}) {
  return {
    uuid: 'device-1',
    editName: 'Steam Deck',
    editDisplayMode: '',
    editAllowClientCommands: true,
    editEnableLegacyOrdering: true,
    editAlwaysUseVirtualDisplay: false,
    editCloseDesktopSteam: false,
    editTemporaryAuthorization: false,
    editPerm: permissionMapping._game_control,
    edit_do: [],
    edit_undo: [],
    ...overrides,
  }
}

function stubFetch(listed = []) {
  const fetchMock = vi.fn((url) => Promise.resolve({
    ok: true,
    json: async () => (url === './api/clients/list'
      ? { status: true, platform: 'linux', named_certs: listed }
      : { status: true }),
  }))
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

describe('device recording approval toggle', () => {
  it('starts unapproved with the real copy, saying no client can use it yet', () => {
    const wrapper = mountToggle({ approved: false })
    const input = wrapper.get('#recording-authorized-device-1')
    expect(input.element.checked).toBe(false)
    expect(input.element.disabled).toBe(false)
    expect(wrapper.text()).toContain(en.pin.recording_authorized)
    expect(wrapper.text()).toContain(en.pin.recording_authorized_desc)
    expect(en.pin.recording_authorized_desc).toMatch(/No client can ask Polaris to record yet/)
    expect(wrapper.find('[data-recording-approval-guest]').exists()).toBe(false)
  })

  it('reports an approval when the host admin checks it', async () => {
    const wrapper = mountToggle({ approved: false })
    await wrapper.get('#recording-authorized-device-1').setValue(true)
    await flushPromises()
    expect(wrapper.emitted('update:approved')).toEqual([[true]])
    expect(wrapper.get('#recording-authorized-device-1').element.checked).toBe(true)
  })

  it('cannot be approved for a guest, and clears an approval loaded for one', async () => {
    const wrapper = mountToggle({ approved: true, guest: true })
    await flushPromises()
    expect(wrapper.emitted('update:approved')).toEqual([[false]])
    const input = wrapper.get('#recording-authorized-device-1')
    expect(input.element.disabled).toBe(true)
    expect(input.element.checked).toBe(false)
    expect(wrapper.get('[data-recording-approval-guest]').text()).toBe(en.pin.recording_authorized_guest)
  })

  it('clears when the device becomes a guest and does not come back when it is permanent again', async () => {
    const wrapper = mountToggle({ approved: true, guest: false })
    await wrapper.setProps({ guest: true })
    await flushPromises()
    expect(wrapper.emitted('update:approved')).toEqual([[false]])
    await wrapper.setProps({ guest: false })
    await flushPromises()
    expect(wrapper.emitted('update:approved')).toEqual([[false]])
    expect(wrapper.get('#recording-authorized-device-1').element.checked).toBe(false)
    expect(wrapper.get('#recording-authorized-device-1').element.disabled).toBe(false)
  })
})

describe('device recording approval requests', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('reads only an explicit true from the host list', async () => {
    stubFetch([
      { name: 'A', uuid: 'a', perm: permissionMapping._all, recording_authorized: true },
      { name: 'B', uuid: 'b', perm: permissionMapping._all, recording_authorized: 'true' },
      { name: 'C', uuid: 'c', perm: permissionMapping._all },
    ])
    const { clients, refreshClients } = useClients()
    await refreshClients()
    expect(clients.value.map((client) => client.recording_authorized)).toEqual([true, false, false])
  })

  it('sends the approval with the device update, beside an unchanged permission mask', async () => {
    const fetchMock = stubFetch()
    const { saveClient } = useClients()
    await saveClient(editedClient({ editRecordingAuthorized: true }))
    const [url, init] = fetchMock.mock.calls[0]
    expect(url).toBe('./api/clients/update')
    const body = JSON.parse(init.body)
    expect(body.recording_authorized).toBe(true)
    expect(body.perm).toBe(permissionMapping._game_control)
    expect(body.temporary_authorization).toBe(false)
  })

  it('always sends a guest as unapproved', async () => {
    const fetchMock = stubFetch()
    const { saveClient } = useClients()
    await saveClient(editedClient({ editTemporaryAuthorization: true, editRecordingAuthorized: true }))
    const body = JSON.parse(fetchMock.mock.calls[0][1].body)
    expect(body.temporary_authorization).toBe(true)
    expect(body.recording_authorized).toBe(false)
  })

  it('leaves the stored choice alone when the editor never loaded it', async () => {
    const fetchMock = stubFetch()
    const { saveClient } = useClients()
    await saveClient(editedClient())
    const body = JSON.parse(fetchMock.mock.calls[0][1].body)
    expect(body).not.toHaveProperty('recording_authorized')
  })

  it('is independent of every access preset and of the permission masks', () => {
    // No permission bit carries recording: the masks and presets are exactly what they were.
    expect(permissionMapping._all).toBe(0x071F1F00)
    expect(Object.keys(permissionMapping).some((key) => key.includes('record'))).toBe(false)
    for (const preset of ['viewer', 'gamepad', 'standard', 'game_control', 'full']) {
      const perm = permissionPresetMask(preset)
      expect(permissionPresetKey(perm)).toBe(preset)
      expect(recordingAuthorizationUpdate(editedClient({ editPerm: perm, editRecordingAuthorized: false })))
        .toEqual({ recording_authorized: false })
      expect(recordingAuthorizationUpdate(editedClient({ editPerm: perm, editRecordingAuthorized: true })))
        .toEqual({ recording_authorized: true })
    }
  })

  it('touches no recorder endpoint while listing or saving devices', async () => {
    const fetchMock = stubFetch([{ name: 'A', uuid: 'a', perm: permissionMapping._all, recording_authorized: true }])
    const { refreshClients, saveClient } = useClients()
    await refreshClients()
    await saveClient(editedClient({ editRecordingAuthorized: false }))
    const urls = fetchMock.mock.calls.map(([url]) => url)
    expect(urls).toEqual(['./api/clients/list', './api/clients/update', './api/clients/list'])
    expect(urls.some((url) => url.includes('/api/recording'))).toBe(false)
  })
})

describe('device editor wiring', () => {
  const source = web('views/PinView.vue')

  it('places the toggle after the permission presets and groups, bound to the guest choice', () => {
    const groups = source.indexOf('v-for="group in permissionGroups"')
    const presets = source.indexOf('v-for="preset in permissionPresets"')
    const toggle = source.indexOf('<RecordingApprovalToggle')
    expect(groups).toBeGreaterThan(-1)
    expect(toggle).toBeGreaterThan(groups)
    expect(toggle).toBeGreaterThan(presets)
    const tag = source.slice(toggle, source.indexOf('/>', toggle))
    expect(tag).toContain('v-model:approved="client.editRecordingAuthorized"')
    expect(tag).toContain(':guest="client.editTemporaryAuthorization === true"')
  })

  it('loads the stored choice on edit and puts it back on cancel', () => {
    expect(source.match(/client\.editRecordingAuthorized = client\.recording_authorized === true/g)).toHaveLength(2)
    const preset = source.slice(source.indexOf('function applyPermissionPreset'), source.indexOf('function colorRangeLabel'))
    expect(preset).not.toContain('Recording')
  })

  it('sends no recorder request from the device editor or its toggle', () => {
    for (const path of ['views/PinView.vue', 'components/RecordingApprovalToggle.vue', 'composables/useClients.js']) {
      expect(web(path)).not.toContain('/api/recording')
    }
  })
})
