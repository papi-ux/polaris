// Polaris Theme System — Space Whale (default) and OLED Dark Galaxy

const STORAGE_KEY = 'polaris-theme'
const THEMES = ['polaris', 'oled']

export function getTheme() {
  return localStorage.getItem(STORAGE_KEY) || 'polaris'
}

export function setTheme(theme) {
  if (!THEMES.includes(theme)) return
  localStorage.setItem(STORAGE_KEY, theme)
  applyTheme(theme)
}

export function toggleTheme() {
  const current = getTheme()
  const next = current === 'polaris' ? 'oled' : 'polaris'
  setTheme(next)
  return next
}

export function applyTheme(theme) {
  if (theme === 'oled') {
    document.documentElement.setAttribute('data-theme', 'oled')
  } else {
    document.documentElement.removeAttribute('data-theme')
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
