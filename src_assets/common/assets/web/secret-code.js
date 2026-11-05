// The version in the sidebar opens the code panel after five presses in a row. A long pause starts
// the count over, so presses scattered across a session never add up.
export const VERSION_PRESSES = 5
export const VERSION_PRESS_GAP_MS = 4000

export function versionPressCounter({ now = () => Date.now() } = {}) {
  let count = 0
  let last = 0
  return () => {
    const at = now()
    count = at - last > VERSION_PRESS_GAP_MS ? 1 : count + 1
    last = at
    if (count < VERSION_PRESSES) return false
    count = 0
    return true
  }
}

// Spelled any way. Anything else is no code at all, and the panel says nothing about it.
export function isStarTrailCode(text) {
  return String(text ?? '').trim().toUpperCase() === 'POLARIS'
}
