import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { clientFamilyLabel, liveClientFamilyLabel, streamClientFamilyLabel } from './client-family.js'

const locale = () => JSON.parse(readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))

describe('client family copy', () => {
  it('tells a Moonlight player what it cannot use without sending it to a Nova screen', () => {
    const note = locale().troubleshooting.snapshot_client_family_moonlight_note
    for (const phrase of ['no media loss', 'PyroWave', 'choosing the launch mode per launch', 'Live Tuning from the client',
      'though Artemis can ask for Host Virtual Display', 'Live Tuning on Mission Control still tunes this stream']) {
      expect(note).toContain(phrase)
    }
    expect(note).not.toContain('Play Setup')
    expect(locale().welcome.launch_mode_per_game).not.toContain('Play Setup')
  })

  it('says pairing without a PIN is for Nova, as the host approves only a Trusted Pair request', () => {
    // nvhttp approves without a PIN only when the request carries trustedpair=1, from a trusted
    // subnet, with the setting on. Only Nova sends it, so "any device on the subnet" was wrong.
    const { config, pin } = locale()
    for (const copy of [config.network_tofu_copy, pin.method_trusted_network_desc, pin.trusted_network_desc]) {
      expect(copy).toContain('Nova')
      expect(copy).toContain('trusted subnet')
    }
    expect(pin.method_trusted_network_desc).toContain('Other clients pair with a PIN')
    expect(pin.trusted_network_desc).toContain('other clients still use a PIN')
  })
})

describe('client family labels', () => {
  it('names Nova only for a device the host marked as Nova', () => {
    expect(clientFamilyLabel({ client_family: 'nova' })).toBe('Nova')
    expect(clientFamilyLabel({ client_family: '' })).toBe('Moonlight / Artemis')
    expect(clientFamilyLabel({})).toBe('Moonlight / Artemis')
  })

  it('finds a live stream client by its paired name', () => {
    const paired = [
      { uuid: 'a', name: 'RetroidPocket6', client_family: 'nova' },
      { uuid: 'b', name: 'Living room TV', client_family: '' },
    ]
    expect(liveClientFamilyLabel('RetroidPocket6', paired)).toBe('Nova')
    expect(liveClientFamilyLabel('Living room TV', paired)).toBe('Moonlight / Artemis')
  })

  it('says nothing when the name is unknown or two kinds of client share it', () => {
    const paired = [
      { uuid: 'a', name: 'Deck', client_family: 'nova' },
      { uuid: 'b', name: 'Deck', client_family: '' },
      { uuid: 'c', name: 'Pixel', client_family: 'nova' },
      { uuid: 'd', name: 'Pixel', client_family: 'nova' },
    ]
    expect(liveClientFamilyLabel('Deck', paired)).toBe('')
    expect(liveClientFamilyLabel('Pixel', paired)).toBe('Nova')
    expect(liveClientFamilyLabel('Unpaired', paired)).toBe('')
    expect(liveClientFamilyLabel('', paired)).toBe('')
    expect(liveClientFamilyLabel('Deck', null)).toBe('')
  })

  it('names the kind a live stream carries, and nothing for a kind it does not know', () => {
    expect(streamClientFamilyLabel('nova')).toBe('Nova')
    expect(streamClientFamilyLabel('moonlight')).toBe('Moonlight / Artemis')
    // Empty is an older host or a session given no kind, never Moonlight.
    expect(streamClientFamilyLabel('')).toBe('')
    expect(streamClientFamilyLabel(undefined)).toBe('')
    expect(streamClientFamilyLabel('artemis')).toBe('')
  })

  it('is what the Devices page, Mission Control and Doctor & Support all show', () => {
    const web = (path) => readFileSync(join(process.cwd(), 'src_assets/common/assets/web', path), 'utf8')
    expect(web('views/PinView.vue')).toContain("import { clientFamilyLabel } from '../client-family.js'")
    const dashboard = web('views/DashboardView.vue')
    expect(dashboard).toContain("import { liveClientFamilyLabel, streamClientFamilyLabel } from '../client-family.js'")
    // The stream's own kind first, so two devices that share a name cannot hide it; the name
    // lookup only for a host that does not send it.
    expect(dashboard).toContain('liveClientFamily(client)')
    expect(dashboard).toContain('streamClientFamilyLabel(client?.client_family) ||')
    expect(web('session-snapshot-rows.js')).toContain("import { streamClientFamilyLabel } from './client-family.js'")
    // The support report's Client line names the kind before the device.
    expect(web('views/TroubleshootingView.vue'))
      .toContain('streamStats.value?.client_type || streamClientFamilyLabel(streamStats.value?.client_family) ||')
  })
})
