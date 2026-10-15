import { mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import MissionStage from './MissionStage.vue'

const cover = './api/covers/image?name=Control'
const hero = './api/apps/control/artwork/hero'
const logo = './api/apps/control/artwork/logo'

// jsdom has no 2D canvas: give the logo check one solid colour to read.
function logoPixels(rgb, alpha = 255) {
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue({
    drawImage() {},
    getImageData: (x, y, width, height) => ({ data: Uint8ClampedArray.from({ length: width * height * 4 }, (_, i) => (i % 4 === 3 ? alpha : rgb[i % 4])) }),
  })
}

function stage(props = {}) {
  return mount(MissionStage, { props: { mode: 'idle', title: 'Control', artSrc: cover, heroSrc: hero, logoSrc: logo, ...props } })
}

afterEach(() => { vi.restoreAllMocks() })

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
    logoPixels([240, 240, 240])
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
    logoPixels([20, 20, 24])
    const dark = stage()
    await dark.get('[data-stage-logo]').trigger('load')
    expect(dark.find('[data-stage-logo]').exists()).toBe(false)
    expect(dark.get('h2 span').classes()).not.toContain('sr-only')
    dark.unmount()
    const missing = stage()
    await missing.get('[data-stage-logo]').trigger('error')
    expect(missing.find('[data-stage-logo]').exists()).toBe(false)
    expect(missing.get('h2 span').classes()).not.toContain('sr-only')
    missing.unmount()
  })

  it('asks for neither image while loading', () => {
    const view = stage({ mode: 'loading', artSrc: '', heroSrc: '', logoSrc: logo })
    expect(view.find('[data-stage-logo]').exists()).toBe(false)
    expect(view.find('.mc-stage-backdrop').exists()).toBe(false)
    view.unmount()
  })
})
