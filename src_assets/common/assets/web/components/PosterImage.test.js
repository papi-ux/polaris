import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'
import PosterImage from './PosterImage.vue'

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
})

