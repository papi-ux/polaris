import { describe, expect, it } from 'vitest'

import {
  appLastLaunched,
  isLaunchReadyApp,
  launchPriorityDetails,
  quickLaunchApps,
  sortAppsByLaunchPriority,
} from './library-launch-priority'

const apps = [
  { uuid: 'broken-old', name: 'Broken Old Favorite', favorite: true },
  { uuid: 'ready-basic', name: 'Ready Basic', cmd: 'game' },
  { uuid: 'recent-ready', name: 'Recent Ready', cmd: 'game', 'last-launched': 1780000000 },
  { uuid: 'favorite-recent', name: 'Favorite Recent', favourite: 'true', cmd: 'game', 'last-launched': 1781000000 },
  { uuid: 'detached-ready', name: 'Detached Ready', detached: ['setsid game'] },
  { uuid: 'desktop', name: 'Desktop' },
]

describe('Library launch priority', () => {
  it('detects launch-ready apps from command, detached command, or Desktop entry', () => {
    expect(isLaunchReadyApp(apps[0])).toBe(false)
    expect(isLaunchReadyApp(apps[1])).toBe(true)
    expect(isLaunchReadyApp(apps[4])).toBe(true)
    expect(isLaunchReadyApp(apps[5])).toBe(true)
  })

  it('uses the newest known launch timestamp across legacy field names', () => {
    expect(appLastLaunched({ 'last-launched': 1, last_launched: 3, lastLaunch: 2 })).toBe(3)
  })

  it('prioritizes favorite, recent, and launch-ready apps before broken entries', () => {
    expect(sortAppsByLaunchPriority(apps).map((app) => app.uuid)).toEqual([
      'favorite-recent',
      'recent-ready',
      'ready-basic',
      'detached-ready',
      'desktop',
      'broken-old',
    ])
  })

  it('keeps the currently running app pinned first for a Moonlight-style resume target', () => {
    expect(quickLaunchApps(apps, { currentApp: 'ready-basic', limit: 3 }).map((app) => app.uuid)).toEqual([
      'ready-basic',
      'favorite-recent',
      'recent-ready',
    ])
  })

  it('surfaces launch status details for rail copy and disabled actions', () => {
    expect(launchPriorityDetails(apps[0])).toMatchObject({ favorite: true, launchReady: false, broken: true })
    expect(launchPriorityDetails(apps[3])).toMatchObject({ favorite: true, recent: true, launchReady: true })
  })
})
