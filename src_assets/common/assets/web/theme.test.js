import { beforeEach, describe, expect, it } from 'vitest'
import {
  applyTheme,
  cycleTheme,
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

  it('registers Miami Nebula alongside the existing skins', () => {
    expect(THEMES.map((theme) => theme.id)).toEqual(['polaris', 'oled', 'miami'])
    expect(THEMES.find((theme) => theme.id === 'miami')).toMatchObject({
      label: 'Miami Nebula',
      shortLabel: 'Miami',
    })
  })

  it('applies non-default skins through the data-theme attribute', () => {
    setTheme('miami')

    expect(getTheme()).toBe('miami')
    expect(localStorage.getItem('polaris-theme')).toBe('miami')
    expect(document.documentElement.getAttribute('data-theme')).toBe('miami')
  })

  it('resolves the next registered skin from any current skin', () => {
    expect(getNextTheme()).toBe('oled')
    expect(getNextTheme('polaris')).toBe('oled')
    expect(getNextTheme('oled')).toBe('miami')
    expect(getNextTheme('miami')).toBe('polaris')
    expect(getNextTheme('mystery-meat')).toBe('polaris')
  })

  it('cycles through every registered skin and wraps back to the default', () => {
    expect(cycleTheme()).toBe('oled')
    expect(document.documentElement.getAttribute('data-theme')).toBe('oled')
    expect(cycleTheme()).toBe('miami')
    expect(document.documentElement.getAttribute('data-theme')).toBe('miami')
    expect(cycleTheme()).toBe('polaris')
    expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
  })

  it('keeps toggleTheme as the backward-compatible cycle API', () => {
    setTheme('oled')

    expect(toggleTheme()).toBe('miami')
    expect(getTheme()).toBe('miami')
    expect(toggleTheme()).toBe('polaris')
    expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
  })

  it('falls back to the default when storage contains an unknown skin', () => {
    localStorage.setItem('polaris-theme', 'mystery-meat')

    expect(getTheme()).toBe('polaris')
    applyTheme(getTheme())
    expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
  })
})
