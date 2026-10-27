import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import PosterImage from './PosterImage.vue'

// jsdom cannot decode images. A test that needs decoding installs this stand-in: each call waits
// until the test settles it with `decodes.at(n).finish()` or `.fail()`.
const nativeDecode = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'decode')
function stubDecode() {
  const decodes = []
  HTMLImageElement.prototype.decode = vi.fn(() => new Promise((finish, fail) => { decodes.push({ finish, fail }) }))
  return decodes
}
afterEach(() => {
  if (nativeDecode) Object.defineProperty(HTMLImageElement.prototype, 'decode', nativeDecode)
  else delete HTMLImageElement.prototype.decode
})

describe('PosterImage', () => {
  it('uses title initials and a labelled fallback without artwork', () => {
    const view = mount(PosterImage, { props: { name: 'Control Ultimate Edition' } })
    expect(view.get('[data-poster-fallback]').text()).toBe('CU')
    expect(view.get('[data-poster-fallback]').attributes('aria-label')).toBe('Control Ultimate Edition artwork unavailable')
    expect(view.find('img').exists()).toBe(false)
    view.unmount()
  })
  it('keeps lazy loading, replaces a failed image, and recovers on new artwork', async () => {
    const view = mount(PosterImage, { props: { name: 'Control', src: '/old.jpg' } })
    expect(view.get('img').attributes('loading')).toBe('lazy')
    expect(view.get('[data-poster-fallback]').attributes('aria-hidden')).toBe('true')
    await view.get('img').trigger('error')
    expect(view.find('img').exists()).toBe(false)
    expect(view.get('[data-poster-fallback]').attributes('aria-hidden')).toBeUndefined()
    await view.setProps({ src: '/new.jpg' })
    expect(view.get('img').attributes('src')).toBe('/new.jpg')
    expect(view.find('.poster-loading').exists()).toBe(true)
    await view.get('img').trigger('load')
    expect(view.find('.poster-loading').exists()).toBe(false)
    await view.setProps({ src: '' })
    expect(view.find('img').exists()).toBe(false)
    expect(view.find('.poster-loading').exists()).toBe(false)
    view.unmount()
  })
  it('handles empty and unicode titles without a broken placeholder', () => {
    const view = mount(PosterImage)
    expect(view.get('[data-poster-fallback]').text()).toBe('?')
    view.unmount()
    const unicode = mount(PosterImage, { props: { name: '🌙 Moon' } })
    expect(unicode.get('[data-poster-fallback]').text()).toBe('🌙M')
    unicode.unmount()
  })
  it('fades a poster in once it has decoded, never for artwork it has since replaced', async () => {
    const decodes = stubDecode()
    const view = mount(PosterImage, { props: { name: 'Control', src: '/old.jpg' } })
    // The image waits, transparent, over the loading shimmer; the initials stay hidden beneath.
    expect(view.get('img').classes()).not.toContain('is-loaded')
    expect(view.get('[data-poster-fallback]').classes()).toContain('is-covered')
    await view.get('img').trigger('load')
    expect(view.get('img').classes()).not.toContain('is-loaded')
    expect(view.find('.poster-loading').exists()).toBe(true)
    // A decode that settles after the artwork changed does not reveal the new artwork.
    await view.setProps({ src: '/new.jpg' })
    decodes[0].finish()
    await flushPromises()
    expect(view.get('img').classes()).not.toContain('is-loaded')
    expect(view.find('.poster-loading').exists()).toBe(true)
    await view.get('img').trigger('load')
    decodes[1].finish()
    await flushPromises()
    expect(view.get('img').classes()).toContain('is-loaded')
    expect(view.find('.poster-loading').exists()).toBe(false)
    view.unmount()
  })
  it('still shows a loaded poster whose decode fails, and brings the initials back for a failed one', async () => {
    const decodes = stubDecode()
    const view = mount(PosterImage, { props: { name: 'Control', src: '/cover.jpg' } })
    await view.get('img').trigger('load')
    decodes[0].fail(new DOMException('decode', 'EncodingError'))
    await flushPromises()
    expect(view.get('img').classes()).toContain('is-loaded')
    await view.setProps({ src: '/missing.jpg' })
    await view.get('img').trigger('error')
    expect(view.get('[data-poster-fallback]').classes()).not.toContain('is-covered')
    view.unmount()
  })
})

