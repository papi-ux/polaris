// Bridge between CSS theme tokens and canvas surfaces (uPlot charts, the
// particle field, SVG gauges), which cannot inherit CSS custom properties.
// Read resolved token values at draw time and re-read them on theme swaps via
// onThemeTokensChange.
import { onThemeChange } from './theme.js'

const TOKEN_NAMES = [
  'ice', 'silver', 'storm', 'twilight', 'deep', 'void',
  'accent', 'accent-2', 'success', 'warning', 'danger', 'info',
]

/**
 * Resolve the current values of the named theme tokens (default: all core
 * tokens) into { ice: '#c8d6e5', ... }. Values come from getComputedStyle, so
 * they reflect the active data-theme.
 */
export function readThemeTokens(names = TOKEN_NAMES) {
  const styles = getComputedStyle(document.documentElement)
  const tokens = {}
  for (const name of names) {
    tokens[name] = styles.getPropertyValue(`--color-${name}`).trim()
  }
  return tokens
}

/**
 * Run the callback with fresh tokens after every theme swap. The callback is
 * NOT invoked for the current theme; call readThemeTokens() yourself for the
 * initial paint. Returns an unsubscribe function.
 */
export function onThemeTokensChange(callback) {
  return onThemeChange(() => {
    callback(readThemeTokens())
  })
}

/**
 * Mix a resolved token color with transparency for canvas fills. Canvas
 * fillStyle predates color-mix, so compute an rgba() string from the hex the
 * tokens resolve to (readThemeTokens only returns plain-hex tokens).
 */
export function withAlpha(color, alpha) {
  const hex = color.replace('#', '')
  if (!/^[0-9a-f]{6}$/i.test(hex)) return color
  const r = parseInt(hex.slice(0, 2), 16)
  const g = parseInt(hex.slice(2, 4), 16)
  const b = parseInt(hex.slice(4, 6), 16)
  return `rgba(${r}, ${g}, ${b}, ${alpha})`
}
