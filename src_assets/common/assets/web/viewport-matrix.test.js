import { describe, expect, it } from 'vitest'
import { POLARIS_VIEWPORTS, getViewportByName } from '../../../../tests/e2e/polaris-viewports.js'

describe('Polaris viewport matrix', () => {
  it('covers desktop, phone, tablet, and handheld dogfood targets', () => {
    expect(POLARIS_VIEWPORTS.map((viewport) => viewport.name)).toEqual([
      'desktop-cockpit',
      'desktop-compact',
      'ultrawide-1080p',
      'phone-portrait',
      'phone-landscape',
      'tablet-portrait',
      'tablet-landscape',
      'handheld-landscape',
    ])

    for (const viewport of POLARIS_VIEWPORTS) {
      expect(viewport.width).toEqual(expect.any(Number))
      expect(viewport.height).toEqual(expect.any(Number))
      expect(viewport.width).toBeGreaterThan(0)
      expect(viewport.height).toBeGreaterThan(0)
    }
  })

  it('keeps the desktop Linux host operator viewport first for screenshot evidence', () => {
    expect(POLARIS_VIEWPORTS[0]).toMatchObject({
      name: 'desktop-cockpit',
      label: 'Desktop Linux host operator',
      width: 1440,
      height: 900,
    })
    expect(getViewportByName('handheld-landscape')).toMatchObject({ width: 1280, height: 720 })
    expect(getViewportByName('tablet-landscape')).toMatchObject({ width: 1024, height: 768 })
  })
})
