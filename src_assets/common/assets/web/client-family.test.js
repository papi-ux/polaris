import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { clientFamilyLabel, liveClientFamilyLabel } from './client-family.js'

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

  it('is what the Devices page and Mission Control both show', () => {
    const web = (path) => readFileSync(join(process.cwd(), 'src_assets/common/assets/web', path), 'utf8')
    expect(web('views/PinView.vue')).toContain("import { clientFamilyLabel } from '../client-family.js'")
    const dashboard = web('views/DashboardView.vue')
    expect(dashboard).toContain("import { liveClientFamilyLabel } from '../client-family.js'")
    expect(dashboard).toContain('liveClientFamily(client.name)')
  })
})
