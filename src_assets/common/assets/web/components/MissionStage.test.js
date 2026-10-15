import { mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import MissionStage from './MissionStage.vue'
import { applyTheme } from '../theme.js'

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

afterEach(() => {
  vi.restoreAllMocks()
  document.documentElement.style.removeProperty('--color-void')
  applyTheme('polaris')
})

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

  it('keeps the title text for white at 40/255 and for a mid tone the old 0.12 average accepted', async () => {
    scrim('#0b0a0a')
    solid([255, 255, 255, 40])
    const faint = stage()
    expect(await loadLogo(faint)).toBe(false)
    faint.unmount()
    // Relative luminance 0.127: about 1.3:1 once the scrim lets a bright background through.
    solid([100, 100, 100, 255])
    const midTone = stage()
    expect(await loadLogo(midTone)).toBe(false)
    midTone.unmount()
  })

  it('keeps the title text for a light grey that would pass only if half the background showed through', async () => {
    // Grey 200 holds 4.7:1 when half of a white background shows; the scrim's thinnest edge lets
    // 85% through, and there it reaches only 2.25:1.
    scrim('#0b0a0a')
    solid([200, 200, 200, 255])
    const view = stage()
    expect(await loadLogo(view)).toBe(false)
    view.unmount()
  })

  it('judges the logo against the theme scrim it is drawn on', async () => {
    // Grey 230 holds 3:1 against the black scrim and its brightest backdrop (3.08:1), not the
    // Polaris ones (2.83:1).
    solid([230, 230, 230, 255])
    scrim('#000000')
    const black = stage()
    expect(await loadLogo(black)).toBe(true)
    black.unmount()
    scrim('#2a2840')
    const polaris = stage()
    expect(await loadLogo(polaris)).toBe(false)
    polaris.unmount()
  })

  it('checks the logo again when the theme changes, showing the text while it does', async () => {
    solid([230, 230, 230, 255])
    scrim('#000')
    applyTheme('oled')
    const view = stage()
    expect(await loadLogo(view)).toBe(true)
    expect(view.get('h2 span').classes()).toContain('sr-only')
    // Polaris's lighter scrim: the same logo no longer passes, so the name shows again.
    scrim('#2a2840')
    applyTheme('polaris')
    await view.vm.$nextTick()
    expect(view.get('h2 span').classes()).not.toContain('sr-only')
    expect(await loadLogo(view)).toBe(false)
    expect(view.get('h2 span').classes()).not.toContain('sr-only')
    // Back on a dark scrim it passes again.
    scrim('#000')
    applyTheme('oled')
    await view.vm.$nextTick()
    expect(await loadLogo(view)).toBe(true)
    view.unmount()
  })

  it('ignores a load from the previous title\'s logo once another title is featured', async () => {
    scrim('#0b0a0a')
    solid([240, 240, 240, 255])
    const view = stage()
    const previous = view.get('[data-stage-logo]').element
    await view.setProps({ title: 'Bravo', logoSrc: './api/apps/bravo/artwork/logo' })
    previous.dispatchEvent(new Event('load'))
    await view.vm.$nextTick()
    expect(view.get('h2 span').text()).toBe('Bravo')
    expect(view.get('h2 span').classes()).not.toContain('sr-only')
    // Bravo's own logo settles Bravo's title.
    expect(await loadLogo(view)).toBe(true)
    view.unmount()
  })

  it('keeps the current logo when the previous title\'s logo reports an error', async () => {
    scrim('#0b0a0a')
    solid([240, 240, 240, 255])
    const view = stage()
    const previous = view.get('[data-stage-logo]').element
    await view.setProps({ title: 'Bravo', logoSrc: './api/apps/bravo/artwork/logo' })
    previous.dispatchEvent(new Event('error'))
    await view.vm.$nextTick()
    expect(view.get('[data-stage-logo]').attributes('src')).toBe('./api/apps/bravo/artwork/logo')
    view.unmount()
  })

  it('checks a logo again when its title returns after a theme change, leaving the background loaded', async () => {
    solid([230, 230, 230, 255])
    scrim('#000')
    applyTheme('oled')
    const view = stage()
    await view.get('[data-stage-hero]').trigger('load')
    expect(await loadLogo(view)).toBe(true)
    await view.setProps({ title: 'Bravo', logoSrc: './api/apps/bravo/artwork/logo' })
    expect(await loadLogo(view)).toBe(true)
    scrim('#2a2840')
    applyTheme('polaris')
    await view.vm.$nextTick()
    await view.setProps({ title: 'Control', logoSrc: logo })
    // The pass Control's logo earned in OLED does not carry over: its name shows until it is checked.
    expect(view.get('h2 span').classes()).not.toContain('sr-only')
    expect(await loadLogo(view)).toBe(false)
    expect(view.get('h2 span').classes()).not.toContain('sr-only')
    // The logo's verdicts never touch the background's own load state.
    expect(view.get('[data-stage-hero]').classes()).toContain('is-loaded')
    view.unmount()
  })

  it('reads the shortened hex the built stylesheet writes for a scrim, such as OLED\'s #000', async () => {
    solid([240, 240, 240, 255])
    scrim('#000')
    const oled = stage()
    expect(await loadLogo(oled)).toBe(true)
    oled.unmount()
    scrim('black')
    const named = stage()
    expect(await loadLogo(named)).toBe(false)
    named.unmount()
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
