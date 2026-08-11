// Polaris Theme System. Five skins matching the Nova client's theme catalog
// (labels, ordering, and palettes from Nova's colors_nova.xml / arrays.xml);
// Nova's Material You is Android-only and has no web analogue.

const STORAGE_KEY = 'polaris-theme'

// preview colors power the picker swatches and must match the theme's token
// block in app.css (window = --color-void, card = --surface-card-base,
// accent = --color-accent). theme.test.js pins them against the CSS.
export const THEMES = [
  {
    id: 'polaris',
    label: 'Polaris Aurora',
    shortLabel: 'Polaris',
    subtitle: 'Polaris blue cockpit · balanced dark streaming shell',
    preview: { window: '#2a2840', card: '#343150', accent: '#7c73ff' },
  },
  {
    id: 'portable-chrome',
    label: 'Portable Chrome',
    shortLabel: 'Portable Chrome',
    subtitle: 'Smoked graphite handheld chrome · PlayStation-symbol accents',
    preview: { window: '#14161a', card: '#1e2228', accent: '#5a93d6' },
  },
  {
    id: 'oled',
    label: 'Console OLED',
    shortLabel: 'OLED',
    subtitle: 'OLED black · high contrast console glow',
    preview: { window: '#000000', card: '#0a0a0e', accent: '#8b80ff' },
  },
  {
    id: 'miami',
    label: 'Miami Nebula',
    shortLabel: 'Miami',
    subtitle: 'Miami neon · flamingo pink glow · cyan night drive',
    preview: { window: '#130817', card: '#241429', accent: '#ff5cab' },
  },
  {
    id: 'high-contrast',
    label: 'High Contrast',
    shortLabel: 'Contrast',
    subtitle: 'Maximum contrast · accessibility-first focus states',
    preview: { window: '#05070c', card: '#0f172a', accent: '#60a5fa' },
  },
]

const DEFAULT_THEME = THEMES[0].id
const THEME_IDS = THEMES.map((theme) => theme.id)

const changeListeners = new Set()

export function getTheme() {
  const storedTheme = localStorage.getItem(STORAGE_KEY)
  return THEME_IDS.includes(storedTheme) ? storedTheme : DEFAULT_THEME
}

export function getThemeMeta(theme) {
  return THEMES.find((item) => item.id === theme) || THEMES[0]
}

export function setTheme(theme) {
  if (!THEME_IDS.includes(theme)) return
  localStorage.setItem(STORAGE_KEY, theme)
  applyTheme(theme)
}

export function getNextTheme(theme = getTheme()) {
  const currentIndex = THEME_IDS.indexOf(theme)
  if (currentIndex === -1) return DEFAULT_THEME
  return THEME_IDS[(currentIndex + 1) % THEME_IDS.length]
}

export function cycleTheme() {
  const next = getNextTheme()
  setTheme(next)
  return next
}

export const toggleTheme = cycleTheme

export function applyTheme(theme) {
  if (theme === DEFAULT_THEME) {
    document.documentElement.removeAttribute('data-theme')
  } else if (THEME_IDS.includes(theme)) {
    document.documentElement.setAttribute('data-theme', theme)
  } else {
    return
  }
  for (const listener of changeListeners) {
    try {
      listener(theme)
    } catch {
      // One listener failing must not break the rest of the theme swap.
    }
  }
}

/**
 * Subscribe to theme changes (canvas surfaces re-read tokens on swap).
 * Returns an unsubscribe function.
 */
export function onThemeChange(listener) {
  changeListeners.add(listener)
  return () => changeListeners.delete(listener)
}

export function initTheme() {
  applyTheme(getTheme())
}

// Backward compat
export const getPreferredTheme = getTheme
export const showActiveTheme = () => {}
export function setupThemeToggleListener() {}
export function loadAutoTheme() { initTheme() }
