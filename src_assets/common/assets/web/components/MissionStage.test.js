import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import MissionStage from './MissionStage.vue'
import { ambienceStylesReady } from '../artwork-tint.js'
import { applyTheme } from '../theme.js'

// The glow's styles load on demand. By default the real loader runs (Vitest hands back the
// stylesheet at once); a test can hold it pending or have it fail.
vi.mock('../artwork-tint.js', async importOriginal => {
  const actual = await importOriginal()
  return { ...actual, ambienceStylesReady: vi.fn(actual.ambienceStylesReady) }
})

const cover = './api/covers/image?name=Control'
const hero = './api/apps/control/artwork/hero'
const logo = './api/apps/control/artwork/logo'

// jsdom has no 2D canvas. The logo check and the cover's tint read their pixels from this stand-in:
// `pixel(index, count)` gives each pixel's [r, g, b, a].
function logoCanvas(pixel) {
  const drawImage = vi.fn()
  const getContext = vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue({
    drawImage,
    getImageData: (x, y, width, height) => ({
      data: Uint8ClampedArray.from({ length: width * height * 4 }, (_, i) => pixel(Math.floor(i / 4), width * height)[i % 4]),
    }),
  })
  return { drawImage, getContext }
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

describe('MissionStage ambience', () => {
  // Tints are kept per cover across stages, so each test features covers of its own.
  const coverOf = name => `./api/covers/image?name=${encodeURIComponent(name)}`
  const live = (props = {}) => stage({ mode: 'live', heroSrc: '', logoSrc: '', ...props })
  const loadCover = view => view.get('[data-stage-artwork] img').trigger('load')
  const glows = view => view.get('[data-mission-stage]').classes().includes('has-ambience')
  const glowsOnceReady = view => vi.waitFor(() => expect(glows(view)).toBe(true))
  const ambienceShown = view => view.find('[data-stage-ambience]').exists() ||
    (view.get('[data-mission-stage]').attributes('style') ?? '').includes('--mc-ambience')

  it('glows live in the cover\'s own colour, read once from the cover already on screen', async () => {
    const { drawImage, getContext } = logoCanvas(() => [200, 60, 40, 255])
    const ember = coverOf('Ember')
    const view = live({ artSrc: ember })
    expect(glows(view)).toBe(false)
    expect(view.find('[data-stage-ambience]').exists()).toBe(false)
    const keyart = view.get('[data-stage-artwork] img').element
    await loadCover(view)
    await glowsOnceReady(view)
    // The hue is kept and the brightest channel set to 150.
    expect(view.get('[data-mission-stage]').attributes('style')).toContain('--mc-ambience: #962d1e')
    expect(view.find('[data-stage-ambience]').exists()).toBe(true)
    // The pixels come from the key art on screen: the image is not requested a second time.
    expect(drawImage).toHaveBeenCalledTimes(1)
    expect(drawImage.mock.calls[0][0]).toBe(keyart)
    // Featured again, the cover's tint comes from the cache.
    await view.setProps({ artSrc: coverOf('Ember elsewhere') })
    await view.setProps({ artSrc: ember })
    await loadCover(view)
    expect(getContext).toHaveBeenCalledTimes(1)
    await glowsOnceReady(view)
    view.unmount()
  })

  it('takes the cover\'s strongest colour rather than a muddy mean of all of them', async () => {
    // Seven pixels in ten deep blue, three a brighter orange: the blue carries more colour overall.
    logoCanvas((index, count) => (index < count * 0.7 ? [30, 50, 140, 255] : [230, 120, 20, 255]))
    const view = live({ artSrc: coverOf('Dusk') })
    await loadCover(view)
    expect(view.get('[data-mission-stage]').attributes('style')).toContain('--mc-ambience: #203696')
    view.unmount()
  })

  it.each([
    ['a grey cover', () => logoCanvas(() => [128, 128, 128, 255])],
    ['a transparent cover', () => logoCanvas(() => [200, 60, 40, 0])],
    ['no 2D context', () => vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null)],
    ['pixels the browser will not share', () => vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue({
      drawImage() {}, getImageData() { throw new DOMException('tainted', 'SecurityError') },
    })],
  ])('keeps the plain stage for %s', async (label, arrange) => {
    arrange()
    const view = live({ artSrc: coverOf(`Plain ${label}`) })
    await loadCover(view)
    await flushPromises()
    expect(glows(view)).toBe(false)
    expect(view.find('[data-stage-ambience]').exists()).toBe(false)
    expect(view.get('[data-mission-stage]').attributes('style') ?? '').not.toContain('--mc-ambience')
    view.unmount()
  })

  it('glows only live, beside the cover it was read from, and never behind the preview', async () => {
    logoCanvas(() => [200, 60, 40, 255])
    vi.mocked(ambienceStylesReady).mockClear()
    const view = stage({ artSrc: coverOf('Fence one'), heroSrc: '', logoSrc: '' })
    await loadCover(view)
    // Idle, the stage features a title from the library; only the title on stream glows, and the
    // glow's styles are not asked for until then. A cover loaded while idle glows once live.
    await flushPromises()
    expect(glows(view)).toBe(false)
    expect(ambienceStylesReady).not.toHaveBeenCalled()
    await view.setProps({ mode: 'live' })
    await glowsOnceReady(view)
    // A late load from the previous title's cover settles nothing for the new one.
    const previous = view.get('[data-stage-artwork] img').element
    await view.setProps({ artSrc: coverOf('Fence two') })
    expect(glows(view)).toBe(false)
    previous.dispatchEvent(new Event('load'))
    await view.vm.$nextTick()
    expect(glows(view)).toBe(false)
    await loadCover(view)
    await glowsOnceReady(view)
    // The live preview takes the art's place, and the glow goes with it.
    await view.setProps({ hideArt: true })
    expect(glows(view)).toBe(false)
    view.unmount()
  })

  it('keeps a bounded number of tints, reading an evicted cover again', async () => {
    const { getContext } = logoCanvas(() => [200, 60, 40, 255])
    const view = live({ artSrc: coverOf('Shelf 0') })
    for (let n = 0; n < 40; n++) {
      await view.setProps({ artSrc: coverOf(`Shelf ${n}`) })
      await loadCover(view)
    }
    expect(getContext).toHaveBeenCalledTimes(40)
    // A recent cover comes from the cache; the earliest has made way and is read again.
    await view.setProps({ artSrc: coverOf('Shelf 38') })
    await loadCover(view)
    expect(getContext).toHaveBeenCalledTimes(40)
    await view.setProps({ artSrc: coverOf('Shelf 0') })
    await loadCover(view)
    expect(getContext).toHaveBeenCalledTimes(41)
    view.unmount()
  })

  it('keeps the plain stage while the glow\'s styles load, and for good when they cannot', async () => {
    logoCanvas(() => [200, 60, 40, 255])
    let finish
    vi.mocked(ambienceStylesReady).mockReturnValueOnce(new Promise(resolve => { finish = resolve }))
    const view = live({ artSrc: coverOf('Lazy one') })
    await loadCover(view)
    // Nothing of the glow is drawn before its styles apply.
    expect(glows(view)).toBe(false)
    expect(ambienceShown(view)).toBe(false)
    // The cover changes while they load; when they arrive, the new cover has no tint yet.
    await view.setProps({ artSrc: coverOf('Lazy two') })
    finish(true)
    await flushPromises()
    expect(glows(view)).toBe(false)
    await loadCover(view)
    expect(glows(view)).toBe(true)
    // Styles that arrive after the stream ends, or after the stage has gone, show nothing.
    const ended = live({ artSrc: coverOf('Lazy three') })
    vi.mocked(ambienceStylesReady).mockReturnValueOnce(new Promise(resolve => { finish = resolve }))
    await loadCover(ended)
    await ended.setProps({ mode: 'idle' })
    finish(true)
    await flushPromises()
    expect(glows(ended)).toBe(false)
    const gone = live({ artSrc: coverOf('Lazy four') })
    vi.mocked(ambienceStylesReady).mockReturnValueOnce(new Promise(resolve => { finish = resolve }))
    await loadCover(gone)
    gone.unmount()
    finish(true)
    await flushPromises()
    // Styles that cannot load leave the stage plain.
    vi.mocked(ambienceStylesReady).mockResolvedValueOnce(false)
    const failed = live({ artSrc: coverOf('Lazy five') })
    await loadCover(failed)
    await flushPromises()
    expect(glows(failed)).toBe(false)
    expect(ambienceShown(failed)).toBe(false)
    view.unmount(); ended.unmount(); failed.unmount()
  })

  it('asks for the glow\'s styles once, and settles a failed load as no glow without retrying', async () => {
    vi.resetModules()
    vi.doMock('../artwork-ambience.css', () => { throw new Error('offline') })
    const { ambienceStylesReady: load } = await vi.importActual('../artwork-tint.js')
    const first = load()
    expect(load()).toBe(first)
    await expect(first).resolves.toBe(false)
    expect(load()).toBe(first)
    vi.doUnmock('../artwork-ambience.css')
  })

  it('keeps the glow\'s styles out of the startup stylesheet, still, and off for reduced motion, High Contrast and Director', () => {
    const web = name => readFileSync(join(process.cwd(), 'src_assets/common/assets/web', name), 'utf8')
    expect(web('app.css')).not.toContain('ambience')
    const css = web('artwork-ambience.css')
    expect(css).not.toContain('infinite')
    const wash = css.slice(css.indexOf('.mc-stage-ambience {'))
    expect(wash.slice(0, wash.indexOf('}'))).not.toContain('animation')
    // Every rule that shows the glow sits behind one motion and theme gate.
    const gate = css.slice(css.indexOf('@media (prefers-reduced-motion: no-preference)'))
    const shown = gate.slice(0, gate.indexOf('\n  }\n'))
    expect(shown).toContain(':root:not([data-theme="high-contrast"], [data-theme="director"]) .mc-stage.has-ambience .mc-stage-ambience')
    expect(css.split('.has-ambience').length).toBe(shown.split('.has-ambience').length)
  })
})
