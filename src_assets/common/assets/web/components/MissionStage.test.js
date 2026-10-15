import { mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import MissionStage from './MissionStage.vue'

const cover = './api/covers/image?name=Control'
const hero = './api/apps/control/artwork/hero'
const logo = './api/apps/control/artwork/logo'

// jsdom has no 2D canvas. The logo check reads its pixels from this stand-in: `pixel(index, count)`
// gives each pixel's [r, g, b, a].
function logoCanvas(pixel) {
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue({
    drawImage() {},
    getImageData: (x, y, width, height) => ({
      data: Uint8ClampedArray.from({ length: width * height * 4 }, (_, i) => pixel(Math.floor(i / 4), width * height)[i % 4]),
    }),
  })
}
const solid = rgba => logoCanvas(() => rgba)
// Each theme's scrim colour is its --color-void token.
const scrim = color => document.documentElement.style.setProperty('--color-void', color)
async function loadLogo(view) {
  await view.get('[data-stage-logo]').trigger('load')
  return view.find('[data-stage-logo]').exists() && view.get('[data-stage-logo]').isVisible()
}

function stage(props = {}) {
  return mount(MissionStage, { props: { mode: 'idle', title: 'Control', artSrc: cover, heroSrc: hero, logoSrc: logo, ...props } })
}

afterEach(() => { vi.restoreAllMocks(); document.documentElement.style.removeProperty('--color-void') })

describe('MissionStage artwork', () => {
  it('puts the Steam background behind the stage and falls back to the softened cover, then to none', async () => {
    const view = stage()
    expect(view.get('.mc-stage-backdrop').attributes('src')).toBe(hero)
    expect(view.get('.mc-stage-backdrop').attributes('data-stage-hero')).toBe('')
    expect(view.get('[data-stage-artwork] img').attributes('src')).toBe(cover)
    await view.get('[data-stage-hero]').trigger('error')
    expect(view.findAll('.mc-stage-backdrop')).toHaveLength(1)
    expect(view.get('.mc-stage-backdrop').attributes('src')).toBe(cover)
    expect(view.find('[data-stage-hero]').exists()).toBe(false)
    await view.get('.mc-stage-backdrop').trigger('error')
    expect(view.find('.mc-stage-backdrop').exists()).toBe(false)
    expect(view.find('[data-stage-artwork]').exists()).toBe(false)
    view.unmount()
  })

  it('keeps a Steam background without a cover, and shows none behind the live preview', async () => {
    const view = stage({ artSrc: '' })
    expect(view.get('[data-stage-hero]').attributes('src')).toBe(hero)
    expect(view.find('[data-stage-artwork]').exists()).toBe(false)
    await view.setProps({ artSrc: cover, hideArt: true })
    expect(view.find('.mc-stage-backdrop').exists()).toBe(false)
    expect(view.find('[data-stage-artwork]').exists()).toBe(false)
    view.unmount()
  })

  it('shows a readable logo in place of the title text, which stays the heading', async () => {
    scrim('#0b0a0a')
    solid([240, 240, 240, 255])
    const view = stage()
    const heading = view.get('h2')
    expect(heading.text()).toBe('Control')
    expect(heading.get('span').classes()).not.toContain('sr-only')
    await view.get('[data-stage-logo]').trigger('load')
    expect(view.get('[data-stage-logo]').isVisible()).toBe(true)
    expect(heading.get('span').classes()).toContain('sr-only')
    expect(heading.text()).toBe('Control')
    expect(view.get('section').attributes('aria-labelledby')).toBe(heading.attributes('id'))
    view.unmount()
  })

  it('keeps the title text for a logo that is too dark for the stage or fails to load', async () => {
    scrim('#0b0a0a')
    solid([20, 20, 24, 255])
    const dark = stage()
    expect(await loadLogo(dark)).toBe(false)
    expect(dark.get('h2 span').classes()).not.toContain('sr-only')
    dark.unmount()
    const missing = stage()
    await missing.get('[data-stage-logo]').trigger('error')
    expect(missing.find('[data-stage-logo]').exists()).toBe(false)
    expect(missing.get('h2 span').classes()).not.toContain('sr-only')
    missing.unmount()
  })

  it('keeps the title text for a light logo drawn almost transparent', async () => {
    scrim('#0b0a0a')
    // White at 1/255 opacity composes to about 1.01:1 against the scrim.
    solid([255, 255, 255, 1])
    const view = stage()
    expect(await loadLogo(view)).toBe(false)
    expect(view.get('h2 span').classes()).not.toContain('sr-only')
    view.unmount()
  })

  it('keeps the title text for a logo with too little solid ink', async () => {
    scrim('#0b0a0a')
    // One opaque white pixel in a hundred, the rest transparent.
    logoCanvas((index) => (index % 100 === 0 ? [255, 255, 255, 255] : [255, 255, 255, 0]))
    const view = stage()
    expect(await loadLogo(view)).toBe(false)
    view.unmount()
  })

  it('accepts light lettering over transparency, the usual shape of a logo', async () => {
    scrim('#0b0a0a')
    logoCanvas((index) => (index % 4 === 0 ? [245, 242, 236, 255] : [0, 0, 0, 0]))
    const view = stage()
    expect(await loadLogo(view)).toBe(true)
    view.unmount()
  })

  it('judges the logo against the theme scrim it is drawn on', async () => {
    // A mid grey holds 3:1 on a black scrim but not on the lighter Polaris one.
    solid([100, 100, 100, 255])
    scrim('#000000')
    const black = stage()
    expect(await loadLogo(black)).toBe(true)
    black.unmount()
    scrim('#2a2840')
    const polaris = stage()
    expect(await loadLogo(polaris)).toBe(false)
    polaris.unmount()
  })

  it.each([
    ['no 2D context', () => vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null)],
    ['a canvas that throws', () => vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(() => { throw new Error('no canvas') })],
    ['pixels that cannot be read', () => vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue({
      drawImage() {}, getImageData() { throw new DOMException('tainted', 'SecurityError') },
    })],
    ['an unknown scrim colour', () => solid([255, 255, 255, 255])],
  ])('keeps the title text when the logo cannot be checked: %s', async (_label, arrange) => {
    if (_label !== 'an unknown scrim colour') scrim('#0b0a0a')
    arrange()
    const view = stage()
    expect(await loadLogo(view)).toBe(false)
    expect(view.get('h2 span').classes()).not.toContain('sr-only')
    view.unmount()
  })

  it('asks for neither image while loading', () => {
    const view = stage({ mode: 'loading', artSrc: '', heroSrc: '', logoSrc: logo })
    expect(view.find('[data-stage-logo]').exists()).toBe(false)
    expect(view.find('.mc-stage-backdrop').exists()).toBe(false)
    view.unmount()
  })
})
