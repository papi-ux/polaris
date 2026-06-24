import { readFileSync } from 'node:fs'
import { mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it } from 'vitest'
import ThemeToggle from './ThemeToggle.vue'
import {
  applyTheme,
  cycleTheme,
  getThemeMeta,
  getNextTheme,
  getTheme,
  setTheme,
  THEMES,
  toggleTheme,
} from './theme.js'

describe('theme skin registry', () => {
  beforeEach(() => {
    localStorage.clear()
    document.documentElement.removeAttribute('data-theme')
  })

  it('registers Portable Chrome after Miami Nebula alongside the existing skins', () => {
    expect(THEMES.map((theme) => theme.id)).toEqual(['polaris', 'oled', 'miami', 'portable-chrome'])
    expect(THEMES.find((theme) => theme.id === 'miami')).toMatchObject({
      label: 'Miami Nebula',
      shortLabel: 'Miami',
    })
    expect(THEMES.find((theme) => theme.id === 'portable-chrome')).toMatchObject({
      label: 'Portable Chrome',
      shortLabel: 'Portable Chrome',
    })
  })

  it('defines Portable Chrome as a Moonlight-grey early-2000s skin', () => {
    const css = readFileSync('src_assets/common/assets/web/app.css', 'utf8')
    const portableChromeCss = css.slice(css.indexOf('/* ── Portable Chrome Skin ── */'))

    expect(portableChromeCss).toContain('Moonlight-grey early-2000s')
    expect(portableChromeCss).toContain('--color-background: #b8c1cc')
    expect(portableChromeCss).toContain('--color-accent: #557395')
    expect(portableChromeCss).toContain('linear-gradient(180deg, rgba(224, 231, 238, 0.90)')
    expect(portableChromeCss).toContain('repeating-linear-gradient(90deg, rgba(255, 255, 255, 0.06)')
  })

  it('adds panel depth while keeping the approved Portable Chrome base palette', () => {
    const css = readFileSync('src_assets/common/assets/web/app.css', 'utf8')
    const portableChromeCss = css.slice(css.indexOf('/* ── Portable Chrome Skin ── */'))

    expect(portableChromeCss).toContain('--color-background: #b8c1cc')
    expect(portableChromeCss).toContain('--color-surface: #d6dde5')
    expect(portableChromeCss).toContain('0 22px 48px rgba(54, 65, 79, 0.28)')
    expect(portableChromeCss).toContain('inset 0 0 0 1px rgba(74, 86, 101, 0.20)')
    expect(portableChromeCss).toContain('box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.66), inset 0 -1px 0 rgba(81, 94, 110, 0.30), 0 8px 20px rgba(72, 84, 99, 0.12);')
  })

  it('dims Portable Chrome whites and restrains green status text for readability', () => {
    const css = readFileSync('src_assets/common/assets/web/app.css', 'utf8')
    const portableChromeCss = css.slice(css.indexOf('/* ── Portable Chrome Skin ── */'))

    expect(portableChromeCss).toContain('--color-background: #b8c1cc')
    expect(portableChromeCss).toContain('--color-surface: #d6dde5')
    expect(portableChromeCss).toContain('linear-gradient(135deg, #d7dde5 0%, #b8c1cc 46%, #98a6b5 100%)')
    expect(portableChromeCss).toContain('[data-theme="portable-chrome"] [class*="text-green-"]')
    expect(portableChromeCss).toContain('color: #315b45 !important;')
    expect(portableChromeCss).toContain('background-color: rgba(58, 102, 78, 0.12) !important;')
    expect(portableChromeCss).toContain('border-color: rgba(49, 91, 69, 0.34) !important;')
  })

  it('applies non-default skins through the data-theme attribute', () => {
    setTheme('portable-chrome')

    expect(getTheme()).toBe('portable-chrome')
    expect(localStorage.getItem('polaris-theme')).toBe('portable-chrome')
    expect(document.documentElement.getAttribute('data-theme')).toBe('portable-chrome')
  })

  it('resolves metadata for registered skins and falls back for unknown skins', () => {
    expect(getThemeMeta('portable-chrome')).toMatchObject({
      id: 'portable-chrome',
      label: 'Portable Chrome',
      shortLabel: 'Portable Chrome',
    })
    expect(getThemeMeta('mystery-meat')).toMatchObject({ id: 'polaris', label: 'Space Whale' })
  })

  it('resolves the next registered skin from any current skin', () => {
    expect(getNextTheme()).toBe('oled')
    expect(getNextTheme('polaris')).toBe('oled')
    expect(getNextTheme('oled')).toBe('miami')
    expect(getNextTheme('miami')).toBe('portable-chrome')
    expect(getNextTheme('portable-chrome')).toBe('polaris')
    expect(getNextTheme('mystery-meat')).toBe('polaris')
  })

  it('cycles through every registered skin and wraps back to the default', () => {
    expect(cycleTheme()).toBe('oled')
    expect(document.documentElement.getAttribute('data-theme')).toBe('oled')
    expect(cycleTheme()).toBe('miami')
    expect(document.documentElement.getAttribute('data-theme')).toBe('miami')
    expect(cycleTheme()).toBe('portable-chrome')
    expect(document.documentElement.getAttribute('data-theme')).toBe('portable-chrome')
    expect(cycleTheme()).toBe('polaris')
    expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
  })

  it('keeps toggleTheme as the backward-compatible cycle API', () => {
    setTheme('oled')

    expect(toggleTheme()).toBe('miami')
    expect(getTheme()).toBe('miami')
    expect(toggleTheme()).toBe('portable-chrome')
    expect(document.documentElement.getAttribute('data-theme')).toBe('portable-chrome')
    expect(toggleTheme()).toBe('polaris')
    expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
  })

  it('falls back to the default when storage contains an unknown skin', () => {
    localStorage.setItem('polaris-theme', 'mystery-meat')

    expect(getTheme()).toBe('polaris')
    applyTheme(getTheme())
    expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
  })

  it('renders toggle copy from the active and next registered skins', async () => {
    setTheme('miami')
    const wrapper = mount(ThemeToggle)

    expect(wrapper.text()).toBe('Miami')
    expect(wrapper.attributes('title')).toBe('Switch to Portable Chrome theme')

    await wrapper.trigger('click')

    expect(getTheme()).toBe('portable-chrome')
    expect(wrapper.text()).toBe('Portable Chrome')
    expect(wrapper.attributes('title')).toBe('Switch to Space Whale theme')
  })

  it('keeps registered skin titles accurate when collapsed', () => {
    setTheme('portable-chrome')
    const wrapper = mount(ThemeToggle, { props: { collapsed: true } })

    expect(wrapper.text()).toBe('')
    expect(wrapper.attributes('title')).toBe('Switch to Space Whale theme')
  })
})
