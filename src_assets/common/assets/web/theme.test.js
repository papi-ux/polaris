import { readFileSync } from 'node:fs'
import { mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it } from 'vitest'
import ThemeToggle from './ThemeToggle.vue'
import {
  applyTheme,
  getThemeMeta,
  getTheme,
  setTheme,
  onThemeChange,
  THEMES,
} from './theme.js'

function hexToRgb(hex) {
  return hex.replace('#', '').match(/.{2}/g).map((channel) => Number.parseInt(channel, 16) / 255)
}

function relativeLuminance(hex) {
  const [red, green, blue] = hexToRgb(hex).map((channel) => (
    channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4
  ))

  return (0.2126 * red) + (0.7152 * green) + (0.0722 * blue)
}

function contrastRatio(foreground, background) {
  const [lighter, darker] = [relativeLuminance(foreground), relativeLuminance(background)].sort((a, b) => b - a)

  return (lighter + 0.05) / (darker + 0.05)
}

const appCss = readFileSync('src_assets/common/assets/web/app.css', 'utf8')

const BLOCK_MARKERS = {
  oled: '/* ── Console OLED Theme',
  miami: '/* ── Miami Nebula Skin',
  'portable-chrome': '/* ── Portable Chrome Skin',
  director: '/* ── Director Skin',
  'high-contrast': '/* ── High Contrast Skin',
}

function themeCss(themeId) {
  if (themeId === 'polaris') {
    // Default theme: tokens live in @theme / :root, before the first skin block.
    return appCss.slice(0, appCss.indexOf('/* ── Console OLED Theme'))
  }
  const markers = Object.values(BLOCK_MARKERS)
  const start = appCss.indexOf(BLOCK_MARKERS[themeId])
  const ends = markers.map((m) => appCss.indexOf(m)).filter((idx) => idx > start)
  return appCss.slice(start, ends.length ? Math.min(...ends) : undefined)
}

function extractTokens(css) {
  const tokens = {}
  for (const [, name, value] of css.matchAll(/--(?:color|surface)-([\w-]+):\s*(#[0-9a-f]{6}|#[0-9a-f]{3})\b/gi)) {
    if (!(name in tokens)) tokens[name] = value.toLowerCase()
  }
  return tokens
}

describe('theme skin registry', () => {
  beforeEach(() => {
    localStorage.clear()
    document.documentElement.removeAttribute('data-theme')
  })

  it('registers the six Nova-matched skins in Nova ordering', () => {
    expect(THEMES.map((theme) => theme.id)).toEqual(['polaris', 'portable-chrome', 'oled', 'miami', 'director', 'high-contrast'])
    expect(THEMES.map((theme) => theme.label)).toEqual([
      'Polaris Aurora',
      'Portable Chrome',
      'Console OLED',
      'Miami Nebula',
      'Director',
      'High Contrast',
    ])
    for (const theme of THEMES) {
      expect(theme.subtitle, `${theme.id} subtitle`).toBeTruthy()
      expect(theme.preview, `${theme.id} preview`).toMatchObject({
        window: expect.stringMatching(/^#[0-9a-f]{6}$/),
        card: expect.stringMatching(/^#[0-9a-f]{6}$/),
        accent: expect.stringMatching(/^#[0-9a-f]{6}$/),
      })
    }
  })

  it('keeps picker preview swatches in sync with each skin token block', () => {
    for (const theme of THEMES) {
      const tokens = extractTokens(themeCss(theme.id))
      expect(theme.preview.window, `${theme.id} preview.window`).toBe(tokens.void)
      expect(theme.preview.accent, `${theme.id} preview.accent`).toBe(tokens.accent)
      expect(theme.preview.card, `${theme.id} preview.card`).toBe(tokens['card-base'])
    }
  })

  it('defines Portable Chrome as Nova dark graphite with PlayStation accents', () => {
    const portable = themeCss('portable-chrome')

    expect(portable).toContain('--color-accent: #5a93d6')
    expect(portable).toContain('--color-void: #14161a')
    expect(portable).toContain('--color-danger: #f28b93')
    expect(portable).toContain('--portable-cross: #5a93d6')
    expect(portable).toContain('--portable-square: #b583b5')
    expect(portable).toContain('--portable-circle: #d4838a')
    expect(portable).toContain('--portable-triangle: #6fbf8a')
    // The light retro chrome is fully retired.
    expect(portable).not.toContain('color-scheme: light')
    expect(portable).not.toContain('repeating-linear-gradient')
    expect(portable).not.toContain('!important')
    expect(appCss).not.toContain('[class*="text-green-"]')
  })

  it('leads Miami Nebula with flamingo pink and keeps water cyan as the secondary', () => {
    const miami = themeCss('miami')

    expect(miami).toContain('--color-accent: #ff5cab')
    expect(miami).toContain('--color-accent-2: #47f3ff')
    expect(miami).toContain('--color-info: #47f3ff')
    expect(miami).toContain('--color-void: #130817')
    expect(themeCss('portable-chrome')).not.toContain('#ff5cab')
  })

  it('uses Nova Director crimson surfaces and tiles, warm copy and black controls', () => {
    const director = themeCss('director')
    for (const value of ['#6e0b17', '#881525', '#9a1b2e', '#5e0a14', '#7b1020', '#261015', '#12090b', '#fff5e8', '#f1d9cd', '#dec0b5', '#ffb7ac']) {
      expect(director).toContain(value)
    }
    expect(director).toContain('--surface-card: #881525')
    // Nova keeps tiles on its crimson elevated surface with black dividers;
    // black stays on controls.
    expect(director).toContain('--surface-raised: #9a1b2e')
    expect(director).toContain('--edge-raised: #12090b')
    expect(director).toContain('--surface-pill: #261015')
    expect(director).toContain('--surface-chip: #261015')
    expect(director).toContain('outline: 2px solid var(--color-ice)')
    // Success and warning stay the shared semantic roles. Info keeps its sky hue,
    // lifted only enough to read on crimson.
    expect(director).not.toMatch(/--color-(success|warning):/)
    expect(director).toContain('--color-info: #7dd3fc')
  })

  it('keeps Director text roles at 4.5:1 on its cards, tiles and hovered tiles', () => {
    const tokens = { ...extractTokens(themeCss('polaris')), ...extractTokens(themeCss('director')) }
    const grounds = ['card-base', 'raised', 'raised-hover']
    const roles = ['ice', 'silver', 'storm', 'support', 'info', 'danger', 'success', 'warning']
    for (const ground of grounds) {
      for (const role of roles) {
        expect(
          contrastRatio(tokens[role], tokens[ground]),
          `director --color-${role} on --surface-${ground} ${tokens[ground]}`,
        ).toBeGreaterThanOrEqual(4.5)
      }
    }
  })

  it('gives Director section kickers the full storm tone without moving other kickers', () => {
    const director = themeCss('director')
    // The shared muted kicker measures 4.10:1 on the crimson card.
    expect(appCss).toMatch(/\.section-kicker\s*\{[^}]*text-storm\/80/)
    expect(director).toMatch(/:where\(\[data-theme="director"\]\) \.section-kicker\s*\{\s*color: var\(--color-storm\);\s*\}/)
    // Hero and console kickers keep their own higher-specificity colours.
    expect(appCss).toMatch(/\[data-dashboard-idle-hero\] \.section-kicker,\s*\.dashboard-live-shell \.section-kicker\s*\{\s*color: var\(--color-accent\);/)
    expect(appCss).toMatch(/\.system-console \.section-kicker\s*\{\s*color: var\(--color-silver\);/)
  })

  it('ships High Contrast with near-opaque surfaces and no glass blur', () => {
    const highContrast = themeCss('high-contrast')

    expect(highContrast).toContain('--color-accent: #60a5fa')
    expect(highContrast).toContain('--color-void: #05070c')
    expect(highContrast).toContain('--color-ice: #ffffff')
    expect(highContrast).toContain('--surface-card: rgba(15, 23, 42, 0.97)')
    expect(highContrast).toContain('backdrop-filter: none')
  })

  it('keeps every skin readable against its own card surface', () => {
    // Ratios are gated against the solid card base; rendered cards sit at
    // high alpha over the darker window, so the solid base is the
    // conservative (lightest) ground for light-on-dark text.
    // silver is --color-foreground (app-wide body text), so it holds the AA
    // body-text floor alongside ice. storm is the deliberately muted caption
    // tier and holds the large-text floor, matching Nova's own muted values.
    const floors = [
      ['ice', 4.5],
      ['silver', 4.5],
      ['storm', 3],
      ['accent', 3],
      ['success', 3],
      ['warning', 3],
      ['danger', 3],
      ['info', 3],
    ]
    const defaults = extractTokens(themeCss('polaris'))
    for (const theme of THEMES) {
      const tokens = { ...defaults, ...extractTokens(themeCss(theme.id)) }
      const surface = tokens['card-base']
      for (const [token, floor] of floors) {
        expect(
          contrastRatio(tokens[token], surface),
          `${theme.id} --color-${token} on ${surface}`,
        ).toBeGreaterThanOrEqual(floor)
      }
    }
  })

  it('gives every skin a supporting-copy role readable on its card and deep grounds', () => {
    // Descriptions, help text and consequences are read to make a decision, so
    // their role holds the 4.5:1 body floor on both grounds that copy sits on.
    // Storm keeps its own meta and caption role and the floor above.
    const defaults = extractTokens(themeCss('polaris'))
    for (const theme of THEMES) {
      const own = extractTokens(themeCss(theme.id))
      const tokens = { ...defaults, ...own }
      expect(own.support, `${theme.id} declares --color-support`).toMatch(/^#[0-9a-f]{6}$/)
      for (const ground of ['card-base', 'deep']) {
        expect(
          contrastRatio(tokens.support, tokens[ground]),
          `${theme.id} --color-support on ${ground} ${tokens[ground]}`,
        ).toBeGreaterThanOrEqual(4.5)
      }
    }
  })

  it('applies non-default skins through the data-theme attribute', () => {
    setTheme('portable-chrome')

    expect(getTheme()).toBe('portable-chrome')
    expect(localStorage.getItem('polaris-theme')).toBe('portable-chrome')
    expect(document.documentElement.getAttribute('data-theme')).toBe('portable-chrome')
  })

  it('resolves metadata for registered skins and falls back for unknown skins', () => {
    expect(getThemeMeta('high-contrast')).toMatchObject({
      id: 'high-contrast',
      label: 'High Contrast',
      shortLabel: 'Contrast',
    })
    expect(getThemeMeta('mystery-meat')).toMatchObject({ id: 'polaris', label: 'Polaris Aurora' })
  })

  it('keeps the pre-paint boot script in lockstep with the theme registry', () => {
    const boot = readFileSync('src_assets/common/assets/web/public/assets/theme-boot.js', 'utf8')
    const header = readFileSync('src_assets/common/assets/web/template_header.html', 'utf8')

    // The boot script must be reachable on the production host: only /assets,
    // /images, and /sw.js are routed by confighttp, so it lives under assets/.
    expect(header).toContain('<script src="./assets/theme-boot.js"></script>')
    expect(boot).toContain("localStorage.getItem('polaris-theme')")
    expect(boot).toContain("theme !== 'polaris'")
    for (const theme of THEMES) {
      expect(boot, `boot id list includes ${theme.id}`).toContain(`'${theme.id}'`)
    }
  })

  it('executes the first-paint boot allowlist for every theme and refuses unknown storage', () => {
    const boot = readFileSync('src_assets/common/assets/web/public/assets/theme-boot.js', 'utf8')
    const run = new Function('localStorage', 'document', boot)
    for (const id of [...THEMES.map(theme => theme.id), 'unregistered', null]) {
      document.documentElement.removeAttribute('data-theme')
      run({ getItem: () => id }, document)
      expect(document.documentElement.getAttribute('data-theme')).toBe(id && id !== 'polaris' && THEMES.some(theme => theme.id === id) ? id : null)
    }
    expect(() => run({ getItem: () => { throw new Error('Storage blocked') } }, document)).not.toThrow()
    expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
  })

  it('re-applying the active skin is a no-op for subscribers', () => {
    const seen = []
    const unsubscribe = onThemeChange((theme) => seen.push(theme))

    setTheme('miami')
    setTheme('miami')
    expect(seen).toEqual(['miami'])

    unsubscribe()
  })

  it('falls back to the default when storage contains an unknown skin', () => {
    localStorage.setItem('polaris-theme', 'mystery-meat')

    expect(getTheme()).toBe('polaris')
    applyTheme(getTheme())
    expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
  })

  it('notifies theme-change subscribers exactly until they unsubscribe', () => {
    const seen = []
    const unsubscribe = onThemeChange((theme) => seen.push(theme))

    setTheme('miami')
    expect(seen).toEqual(['miami'])

    unsubscribe()
    setTheme('oled')
    expect(seen).toEqual(['miami'])
  })
})

describe('theme picker', () => {
  beforeEach(() => {
    localStorage.clear()
    document.documentElement.removeAttribute('data-theme')
  })

  it('opens a listbox with every registered skin and marks the current one', async () => {
    setTheme('miami')
    const wrapper = mount(ThemeToggle, { attachTo: document.body })

    expect(wrapper.find('button[aria-haspopup="listbox"]').text()).toContain('Appearance')

    expect(wrapper.find('[role="listbox"]').exists()).toBe(false)
    await wrapper.find('button[aria-haspopup="listbox"]').trigger('click')

    const options = wrapper.findAll('[role="option"]')
    expect(options).toHaveLength(THEMES.length)
    for (const theme of THEMES) {
      expect(options.some((option) => option.text().includes(theme.label)), theme.label).toBe(true)
    }
    const current = wrapper.find('[role="option"][aria-selected="true"]')
    expect(current.text()).toContain('Miami Nebula')
    expect(current.text()).toContain('Current')
    wrapper.unmount()
  })

  it('applies a chosen skin and closes the listbox', async () => {
    const wrapper = mount(ThemeToggle, { attachTo: document.body })

    await wrapper.find('button[aria-haspopup="listbox"]').trigger('click')
    const oledOption = wrapper.findAll('[role="option"]').find((option) => option.text().includes('Console OLED'))
    await oledOption.trigger('click')

    expect(getTheme()).toBe('oled')
    expect(document.documentElement.getAttribute('data-theme')).toBe('oled')
    expect(wrapper.find('[role="listbox"]').exists()).toBe(false)
    expect(wrapper.find('button[aria-haspopup="listbox"]').text()).toContain('OLED')
    wrapper.unmount()
  })

  it('keeps an accessible name while collapsed and hides the label text', () => {
    setTheme('high-contrast')
    const wrapper = mount(ThemeToggle, { props: { collapsed: true } })

    const button = wrapper.find('button[aria-haspopup="listbox"]')
    expect(button.attributes('aria-label')).toBe('Choose theme')
    expect(button.text()).toBe('')
    expect(button.attributes('title')).toBe('Theme: High Contrast')
    wrapper.unmount()
  })
})
