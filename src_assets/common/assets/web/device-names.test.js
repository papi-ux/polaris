import { describe, expect, it } from 'vitest'
import { deviceBaseName, deviceNameLabels } from './device-names.js'
import { formatClientTimestamp } from './client-timestamps.js'

const t = (key, params) => key === '_common.device_name_paired'
  ? `${params.name} (paired ${params.paired})`
  : `${params.name} (${params.number})`

describe('device name labels', () => {
  it('leaves unique names exactly as they are', () => {
    const labels = deviceNameLabels([
      { uuid: 'a', name: 'RetroidPocket6', paired_at: 1789593894 },
      { uuid: 'b', name: 'Pixel10Pro', friendly_name: 'Pixel 10 Pro', paired_at: 1789594614 },
    ], { t })
    expect(labels.get('a')).toBe('RetroidPocket6')
    expect(labels.get('b')).toBe('Pixel 10 Pro')
  })

  it('adds when each paired, in pairing order, when names collide', () => {
    const later = { uuid: 'z', name: 'RetroidPocket6', paired_at: 1789600000 }
    const earlier = { uuid: 'y', name: 'retroidpocket6', paired_at: 1789593894 }
    const labels = deviceNameLabels([later, earlier], { t, locale: 'en-US', timeZone: 'UTC' })
    expect(labels.get('y')).toBe(`retroidpocket6 (paired ${formatClientTimestamp(earlier.paired_at, 'en-US', 'UTC')})`)
    expect(labels.get('z')).toBe(`RetroidPocket6 (paired ${formatClientTimestamp(later.paired_at, 'en-US', 'UTC')})`)
  })

  it('numbers them when the pairing time is missing or repeats', () => {
    const labels = deviceNameLabels([
      { uuid: 'b', name: 'Deck', paired_at: 1789593894 },
      { uuid: 'a', name: 'Deck', paired_at: 1789593894 },
      { uuid: 'c', name: 'Deck' },
    ], { t, locale: 'en-US', timeZone: 'UTC' })
    expect(labels.get('c')).toBe('Deck (1)')
    expect(labels.get('a')).toBe('Deck (2)')
    expect(labels.get('b')).toBe('Deck (3)')
  })

  it('reads the friendly name first and skips entries without an id', () => {
    expect(deviceBaseName({ friendly_name: ' Retroid Pocket 6 ', name: 'RetroidPocket6' })).toBe('Retroid Pocket 6')
    expect(deviceNameLabels([{ name: 'No id' }, null], { t }).size).toBe(0)
  })
})
