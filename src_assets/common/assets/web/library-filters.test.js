import { describe, expect, it } from 'vitest'
import { filterLibraryApps } from './library-filters'

const apps = [
  { uuid: 'desktop', name: 'Desktop', source: 'manual', cmd: 'desktop-session' },
  { uuid: 'steam-bpm', name: 'Steam Big Picture', source: 'steam', cmd: 'steam -gamepadui' },
  { uuid: 'raiders', name: 'ARC Raiders', source: 'steam', 'game-category': 'fast_action', cmd: 'steam steam://rungameid/1808500' },
  { uuid: 'heroic-game', name: 'Alan Wake 2', source: 'heroic', 'game-category': 'story', cmd: 'heroic launch' },
  { name: 'Unsaved Draft' },
]

describe('Library filters', () => {
  it('excludes unsaved entries and searches across useful app fields', () => {
    const filtered = filterLibraryApps(apps, { query: 'gamepad', filter: 'all', currentApp: '' })

    expect(filtered.map((app) => app.uuid)).toEqual(['steam-bpm'])
  })

  it('filters by source and fast-action category', () => {
    expect(filterLibraryApps(apps, { query: '', filter: 'steam', currentApp: '' }).map((app) => app.uuid))
      .toEqual(['steam-bpm', 'raiders'])
    expect(filterLibraryApps(apps, { query: '', filter: 'fast_action', currentApp: '' }).map((app) => app.uuid))
      .toEqual(['raiders'])
  })

  it('filters to the running app when requested', () => {
    const filtered = filterLibraryApps(apps, { query: '', filter: 'running', currentApp: 'heroic-game' })

    expect(filtered.map((app) => app.uuid)).toEqual(['heroic-game'])
  })
})
