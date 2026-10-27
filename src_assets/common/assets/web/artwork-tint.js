// The colour a cover lends the stage's glow, read once from the cover the page has already
// loaded: the image element on screen is drawn small onto a canvas, so nothing is fetched again.
// Its colourful pixels are grouped by hue and the strongest run of neighbouring hues gives the
// tint, so a blue cover with an orange title glows blue rather than a muddy mean of the two.
//
// A cover with too little colour, or one whose pixels cannot be read (a cross-origin image
// without CORS, no 2D context, a failed draw), has no tint: the stage keeps its theme colours.
const SIZE = 24
const HUES = 12
// Pixels nearer grey than this carry no hue worth glowing in.
const MIN_CHROMA = 24
// Under one opaque pixel in twenty with colour, a cover counts as grey.
const MIN_COLOURFUL = 0.05
// Every tint has the same brightest channel, so a pale cover and a dark one glow alike. It stays
// under the brightest backdrop the stage's logo screen allows for (0.6 × 255), so the glow over
// the artwork never lightens it past that bound.
const PEAK = 150
const LIMIT = 32

// Most recently used last; the oldest makes way past LIMIT. Unreadable covers are kept too, so
// one is not drawn again each time it is featured.
const tints = new Map()

function readTint(img) {
  try {
    const canvas = document.createElement('canvas')
    canvas.width = SIZE; canvas.height = SIZE
    const context = canvas.getContext('2d')
    if (!context) return ''
    context.drawImage(img, 0, 0, SIZE, SIZE)
    const pixels = context.getImageData(0, 0, SIZE, SIZE).data
    const groups = Array.from({ length: HUES }, () => ({ weight: 0, r: 0, g: 0, b: 0 }))
    let opaque = 0, colourful = 0
    for (let i = 0; i < pixels.length; i += 4) {
      if (pixels[i + 3] < 128) continue
      opaque++
      const r = pixels[i], g = pixels[i + 1], b = pixels[i + 2]
      const max = Math.max(r, g, b)
      const chroma = max - Math.min(r, g, b)
      if (chroma < MIN_CHROMA) continue
      colourful++
      const hue = max === r ? (g - b) / chroma : max === g ? 2 + (b - r) / chroma : 4 + (r - g) / chroma
      // Vivid pixels count for more than dull ones.
      const group = groups[Math.floor(((hue + 6) % 6) * HUES / 6) % HUES]
      group.weight += chroma
      group.r += r * chroma; group.g += g * chroma; group.b += b * chroma
    }
    if (!opaque || colourful < opaque * MIN_COLOURFUL) return ''
    let best = null
    for (let at = 0; at < HUES; at++) {
      const run = [HUES - 1, 0, 1].map(offset => groups[(at + offset) % HUES])
      const weight = run.reduce((sum, group) => sum + group.weight, 0)
      if (!best || weight > best.weight) best = { run, weight }
    }
    const mean = ['r', 'g', 'b'].map(channel => best.run.reduce((sum, group) => sum + group[channel], 0) / best.weight)
    const peak = Math.max(...mean)
    return `#${mean.map(value => Math.round(value * PEAK / peak).toString(16).padStart(2, '0')).join('')}`
  } catch {
    return ''
  }
}

// The tint for a loaded cover as '#rrggbb', or '' for none. Pass the cover's src as it was set on
// the image; each cover is read once while it stays among the recent ones.
export function artworkTint(img, src) {
  if (!src) return ''
  if (!tints.has(src)) {
    tints.set(src, readTint(img))
    if (tints.size > LIMIT) tints.delete(tints.keys().next().value)
  }
  const tint = tints.get(src)
  tints.delete(src)
  tints.set(src, tint)
  return tint
}

// The glow's styles (artwork-ambience.css) stay out of the startup stylesheet. They load once, on
// first use, through Vite's dynamic CSS loading, and every stage shares that one request. Resolves
// true once they apply, or false when they cannot load: the stage then stays plain, with no retry.
let ambienceStyles = null
export function ambienceStylesReady() {
  if (!ambienceStyles) ambienceStyles = import('./artwork-ambience.css').then(() => true, () => false)
  return ambienceStyles
}
