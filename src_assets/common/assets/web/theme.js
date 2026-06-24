// Polaris Theme System — Space Whale (default), OLED Dark Galaxy, Miami Nebula, and Portable Chrome

const STORAGE_KEY = 'polaris-theme'

export const THEMES = [
  { id: 'polaris', label: 'Space Whale', shortLabel: 'Polaris' },
  { id: 'oled', label: 'OLED Dark Galaxy', shortLabel: 'OLED' },
  { id: 'miami', label: 'Miami Nebula', shortLabel: 'Miami' },
  { id: 'portable-chrome', label: 'Portable Chrome', shortLabel: 'Portable Chrome' },
]

const DEFAULT_THEME = THEMES[0].id
const THEME_IDS = THEMES.map((theme) => theme.id)

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
  }
}

export function initTheme() {
  applyTheme(getTheme())
}

// Backward compat
export const getPreferredTheme = getTheme
export const showActiveTheme = () => {}
export function setupThemeToggleListener() {}
export function loadAutoTheme() { initTheme() }
