import { readFileSync } from 'node:fs'
import { mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it } from 'vitest'
import { nextTick } from 'vue'
import ThemeToggle from './ThemeToggle.vue'
import {
  applyTheme,
  getThemeMeta,
  getTheme,
  setTheme,
  onThemeChange,
  THEMES,
} from './theme.js'
import { vi } from 'vitest'
import { embeddedClient, embeddedTheme, initClientEmbed } from './client-embed.js'

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
      '< Congratulations, Director >',
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

  it('keeps the Director id and names it for Control on Polaris', () => {
    // The visible name is exact text; the id, storage key and boot allowlist stay 'director'.
    expect(getThemeMeta('director')).toMatchObject({
      id: 'director',
      label: '< Congratulations, Director >',
      shortLabel: '< Congratulations, Director >',
    })
    setTheme('director')
    expect(localStorage.getItem('polaris-theme')).toBe('director')
    expect(document.documentElement.getAttribute('data-theme')).toBe('director')
  })

  it('makes Director charcoal-dominant with red kept to signals and focus', () => {
    const director = themeCss('director')
    const tokens = { ...extractTokens(themeCss('polaris')), ...extractTokens(director) }
    const channels = (hex) => hexToRgb(hex).map((channel) => Math.round(channel * 255))
    // Every stage, card, tile and control surface is near-black and nearly neutral, so the page
    // reads dark rather than tinted.
    for (const surface of ['void', 'deep', 'card-base', 'raised', 'raised-hover', 'pill', 'chip']) {
      const rgb = channels(tokens[surface])
      expect(relativeLuminance(tokens[surface]), `${surface} ${tokens[surface]} is near-black`).toBeLessThanOrEqual(0.02)
      expect(Math.max(...rgb) - Math.min(...rgb), `${surface} ${tokens[surface]} is near-neutral`).toBeLessThanOrEqual(12)
    }
    // Copy is warm white, never cool.
    for (const role of ['ice', 'silver']) {
      const [red, , blue] = channels(tokens[role])
      expect(red, `${role} is warm`).toBeGreaterThan(blue)
    }
    // The accent is a clear red that reads as a focus ring or toggle on the darkest stage and
    // on cards (3:1 non-text), while danger is set apart from it by lightness.
    const [red, green, blue] = channels(tokens.accent)
    expect(red).toBeGreaterThan(2.5 * green)
    expect(red).toBeGreaterThan(2.5 * blue)
    for (const ground of ['void', 'card-base', 'raised']) {
      expect(contrastRatio(tokens.accent, tokens[ground]), `accent on ${ground}`).toBeGreaterThanOrEqual(3)
    }
    expect(relativeLuminance(tokens.danger)).toBeGreaterThanOrEqual(1.5 * relativeLuminance(tokens.accent))
    expect(director).toMatch(/\.settings-input:focus\s*\{[^}]*outline: 2px solid var\(--color-accent\)/)
    expect(director).toMatch(/\.focus-ring\s*\{\s*@apply focus-visible:ring-accent/)
    // An input's own edge stays a visible boundary (3:1) on the card it sits on.
    const edge = /\.settings-input\s*\{[^}]*border-color: color-mix\(in srgb, var\(--color-ice\) (\d+)%, transparent\)/.exec(director)
    expect(edge).not.toBeNull()
    const share = Number(edge[1]) / 100
    const composite = '#' + channels(tokens.ice).map((channel, index) => Math.round(share * channel + (1 - share) * channels(tokens['card-base'])[index])
      .toString(16).padStart(2, '0')).join('')
    expect(contrastRatio(composite, tokens['card-base']), `input edge ${composite}`).toBeGreaterThanOrEqual(3)
    // Success and warning stay the shared semantic roles.
    expect(director).not.toMatch(/--color-(success|warning):/)
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
    // The shared kicker is muted to 80%; Director keeps the full storm tone.
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

  it('shows the theme Nova asked for during a Host Console visit, and a theme picked there saves as usual', () => {
    const ids = THEMES.map(theme => theme.id)
    localStorage.setItem('polaris-theme', 'miami')
    try {
      initClientEmbed({ search: '?client=nova&theme=director', themes: ids })
      expect(getTheme()).toBe('director')
      expect(localStorage.getItem('polaris-theme')).toBe('miami')
      setTheme('oled')
      expect(localStorage.getItem('polaris-theme')).toBe('oled')
      expect(embeddedTheme()).toBe('')
      expect(getTheme()).toBe('oled')
    } finally {
      initClientEmbed({ search: '', storage: null, themes: ids })
      sessionStorage.clear()
      applyTheme('polaris')
    }
  })

  it('paints the theme Nova named before first paint, and then the visit\'s', () => {
    const boot = readFileSync('src_assets/common/assets/web/public/assets/theme-boot.js', 'utf8')
    const run = new Function('localStorage', 'document', boot)
    const stored = { getItem: () => 'miami' }
    try {
      history.replaceState(null, '', '/?client=nova&theme=director#/')
      document.documentElement.removeAttribute('data-theme')
      run(stored, document)
      expect(document.documentElement.getAttribute('data-theme')).toBe('director')
      // Nova's own Aurora is the default theme, which the page shows with no attribute at all,
      // whatever the browser saved.
      history.replaceState(null, '', '/?client=nova&theme=polaris#/')
      document.documentElement.removeAttribute('data-theme')
      run(stored, document)
      expect(document.documentElement.hasAttribute('data-theme')).toBe(false)
      // After the first load the visit's session storage carries it; an unknown theme falls back.
      history.replaceState(null, '', '/#/apps')
      sessionStorage.setItem('polaris-embed', JSON.stringify({ client: 'nova', theme: 'oled' }))
      run(stored, document)
      expect(document.documentElement.getAttribute('data-theme')).toBe('oled')
      sessionStorage.setItem('polaris-embed', JSON.stringify({ client: 'nova', theme: 'crimson' }))
      run(stored, document)
      expect(document.documentElement.getAttribute('data-theme')).toBe('miami')
    } finally {
      history.replaceState(null, '', '/')
      sessionStorage.clear()
      document.documentElement.removeAttribute('data-theme')
    }
  })

  it('keeps a theme picked in the console across a reload of the address the page is on', () => {
    const ids = THEMES.map(theme => theme.id)
    const boot = readFileSync('src_assets/common/assets/web/public/assets/theme-boot.js', 'utf8')
    const run = new Function('localStorage', 'document', boot)
    localStorage.setItem('polaris-theme', 'miami')
    try {
      history.replaceState(null, '', '/?client=nova&theme=director&lang=en#/apps')
      initClientEmbed({ themes: ids })
      expect(getTheme()).toBe('director')
      setTheme('oled')
      // A reload starts again from the address the page is on now.
      initClientEmbed({ themes: ids })
      expect([embeddedClient(), getTheme()]).toEqual(['nova', 'oled'])
      document.documentElement.removeAttribute('data-theme')
      run(localStorage, document)
      expect(document.documentElement.getAttribute('data-theme')).toBe('oled')
      // The rest of the address stays.
      expect(new URLSearchParams(location.search).get('lang')).toBe('en')
      expect(location.hash).toBe('#/apps')
    } finally {
      history.replaceState(null, '', '/')
      initClientEmbed({ search: '', storage: null, themes: ids })
      sessionStorage.clear()
      applyTheme('polaris')
    }
  })

  it('keeps Nova mode and a picked theme across a reload when session storage is refused', () => {
    const ids = THEMES.map(theme => theme.id)
    const boot = readFileSync('src_assets/common/assets/web/public/assets/theme-boot.js', 'utf8')
    const run = new Function('localStorage', 'document', boot)
    const refuse = vi.spyOn(sessionStorage, 'setItem').mockImplementation(() => { throw new Error('blocked') })
    localStorage.setItem('polaris-theme', 'miami')
    try {
      history.replaceState(null, '', '/?client=nova&theme=director#/')
      initClientEmbed({ themes: ids })
      expect(getTheme()).toBe('director')
      setTheme('oled')
      initClientEmbed({ themes: ids })
      expect([embeddedClient(), getTheme()]).toEqual(['nova', 'oled'])
      document.documentElement.removeAttribute('data-theme')
      run(localStorage, document)
      expect(document.documentElement.getAttribute('data-theme')).toBe('oled')
    } finally {
      refuse.mockRestore()
      history.replaceState(null, '', '/')
      initClientEmbed({ search: '', storage: null, themes: ids })
      sessionStorage.clear()
      applyTheme('polaris')
    }
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

  // The open list is rendered on <body> (so a scrolling sidebar cannot clip it) and anchored to its
  // button's on-screen box, so these cases give the button real geometry and look for the list there.
  function placeButton(wrapper, box = { top: 600, bottom: 640, left: 12, right: 244 }) {
    const button = wrapper.find('button[aria-haspopup="listbox"]').element
    button.getBoundingClientRect = () => ({ ...box, x: box.left, y: box.top, width: box.right - box.left, height: box.bottom - box.top })
  }

  it('opens a listbox with every registered skin and marks the current one', async () => {
    setTheme('miami')
    const wrapper = mount(ThemeToggle, { attachTo: document.body })
    placeButton(wrapper)

    expect(wrapper.find('button[aria-haspopup="listbox"]').text()).toContain('Appearance')

    expect(document.querySelector('[role="listbox"]')).toBeNull()
    await wrapper.find('button[aria-haspopup="listbox"]').trigger('click')

    const options = [...document.querySelectorAll('[role="option"]')]
    expect(options).toHaveLength(THEMES.length)
    for (const theme of THEMES) {
      expect(options.some((option) => option.textContent.includes(theme.label)), theme.label).toBe(true)
    }
    const current = document.querySelector('[role="option"][aria-selected="true"]')
    expect(current.textContent).toContain('Miami Nebula')
    expect(current.textContent).toContain('Current')
    wrapper.unmount()
    expect(document.querySelector('[role="listbox"]')).toBeNull()
  })

  it('applies a chosen skin and closes the listbox', async () => {
    const wrapper = mount(ThemeToggle, { attachTo: document.body })
    placeButton(wrapper)

    await wrapper.find('button[aria-haspopup="listbox"]').trigger('click')
    const oledOption = [...document.querySelectorAll('[role="option"]')].find((option) => option.textContent.includes('Console OLED'))
    oledOption.click()
    await nextTick()

    expect(getTheme()).toBe('oled')
    expect(document.documentElement.getAttribute('data-theme')).toBe('oled')
    expect(document.querySelector('[role="listbox"]')).toBeNull()
    expect(wrapper.find('button[aria-haspopup="listbox"]').text()).toContain('OLED')
    wrapper.unmount()
  })

  it('does not float the list once its button is off screen', async () => {
    const wrapper = mount(ThemeToggle, { attachTo: document.body })
    placeButton(wrapper, { top: window.innerHeight + 40, bottom: window.innerHeight + 80, left: 12, right: 244 })
    await wrapper.find('button[aria-haspopup="listbox"]').trigger('click')
    expect(document.querySelector('[role="listbox"]')).toBeNull()
    expect(wrapper.find('button[aria-haspopup="listbox"]').attributes('aria-expanded')).toBe('false')
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
