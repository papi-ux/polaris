<template>
  <canvas aria-hidden="true" ref="canvas" class="space-particles inset-0 pointer-events-none" :class="absolute ? 'absolute max-w-full' : 'fixed'" :style="{ zIndex: absolute ? 0 : 0 }" />
</template>

<script setup>
import { ref, watch, onMounted, onUnmounted } from 'vue'
import { getTheme, onThemeChange } from '../theme.js'
import { readThemeTokens } from '../theme-bridge.js'

const props = defineProps({
  dense: { type: Boolean, default: false },
  absolute: { type: Boolean, default: false },
  mode: { type: String, default: '' },
})

const canvas = ref(null)
let ctx = null
let animId = null
let backdrop = null
let backdropCtx = null
let pixelRatio = 1
let lastFrameTime = null
// Two canvases at most, each capped at four million pixels / 8192 per side.
// Bounded object counts and 30 Hz draws keep the field independent of refresh rate.
const MAX_DPR = 2
const MAX_PIXELS = 4_000_000
const MAX_SIDE = 8192
const FRAME_INTERVAL = 1000 / 30
// A mode's star count is its density over a 1280x800 surface. Smaller surfaces
// draw proportionally fewer stars, never below a quarter of the mode's count.
const REFERENCE_AREA = 1280 * 800
const MIN_DENSITY_SHARE = 0.25
let particles = []
let shootingStars = []
let nebulae = []
let galaxyObjects = []
let w = 0
let h = 0
let currentMode = 'balanced'
let currentTheme = 'polaris'
let currentConfig = null
let currentPalette = null
let resizeObserver = null
let resizeFrame = null
let resizePending = false
let pendingW = 0
let pendingH = 0
let motionMediaQuery = null
let prefersReducedMotion = false
let pageVisible = true

function randomBetween(min, max) {
  return Math.random() * (max - min) + min
}

function resolveMode() {
  if (props.mode) return props.mode
  return props.dense ? 'dense' : 'balanced'
}

function getModeConfig(mode) {
  if (mode === 'dense') {
    return {
      count: 420,
      brightChance: 0.24,
      dimSize: [0.35, 1.9],
      brightSize: [1.1, 3.4],
      dimOpacity: [0.07, 0.42],
      brightOpacity: [0.4, 0.82],
      driftX: 0.07,
      driftY: 0.07,
      twinkle: [0.004, 0.012],
      nebulaCount: 9,
      galaxyCount: 6,
      shootingChance: 0.0025,
      maxShootingStars: 2,
      sparkleThreshold: 0.76,
    }
  }

  if (mode === 'cinematic') {
    return {
      count: 340,
      brightChance: 0.23,
      dimSize: [0.38, 1.75],
      brightSize: [1.05, 3.15],
      dimOpacity: [0.08, 0.38],
      brightOpacity: [0.34, 0.74],
      driftX: 0.09,
      driftY: 0.09,
      twinkle: [0.004, 0.01],
      nebulaCount: 7,
      galaxyCount: 5,
      shootingChance: 0.002,
      maxShootingStars: 2,
      sparkleThreshold: 0.78,
    }
  }

  return {
    count: 280,
    brightChance: 0.2,
    dimSize: [0.35, 1.55],
    brightSize: [0.95, 2.75],
    dimOpacity: [0.08, 0.33],
    brightOpacity: [0.28, 0.62],
    driftX: 0.1,
    driftY: 0.1,
    twinkle: [0.0035, 0.008],
    nebulaCount: 6,
    galaxyCount: 4,
    shootingChance: 0.001,
    maxShootingStars: 1,
    sparkleThreshold: 0.8,
  }
}

// Per-theme particle strength, mirroring Nova's librarySurfaces particle
// table: OLED stays true black; Director keeps its near-black stage still.
const THEME_PARTICLE_ALPHA = {
  polaris: 1,
  'portable-chrome': 0.44,
  oled: 0,
  miami: 0.68,
  director: 0,
  'high-contrast': 0.28,
}

function getThemeParticleAlpha(theme) {
  return THEME_PARTICLE_ALPHA[theme] ?? 1
}

function hexTriplet(hex, fallback) {
  const clean = String(hex || '').replace('#', '')
  if (!/^[0-9a-f]{6}$/i.test(clean)) return fallback
  return [
    parseInt(clean.slice(0, 2), 16),
    parseInt(clean.slice(2, 4), 16),
    parseInt(clean.slice(4, 6), 16),
  ]
}

function brighten(rgb, factor) {
  return rgb.map((channel) => Math.round(channel + (255 - channel) * factor))
}

// Star, glow, and streak colors come from the active theme's tokens so the
// canvas cannot drift from the palette; only the non-token art direction
// (nebula hue mixes, radii, intensities) is declared per theme.
const THEME_ART = {
  polaris: {
    nebulae: (glow) => [glow, [98, 132, 190], [88, 116, 146]],
    nebulaOpacity: [0.025, 0.055],
    nebulaRadius: [120, 300],
    galaxyRadius: [66, 150],
    galaxyGlow: [0.04, 0.08],
    galaxyCore: [0.07, 0.14],
    galaxyRing: [0.11, 0.2],
    starBloom: 0.12,
  },
  miami: {
    nebulae: (glow, accent2) => [glow, accent2, [190, 84, 150], [108, 60, 111]],
    nebulaOpacity: [0.026, 0.06],
    nebulaRadius: [130, 320],
    galaxyRadius: [70, 160],
    galaxyGlow: [0.05, 0.09],
    galaxyCore: [0.08, 0.16],
    galaxyRing: [0.12, 0.22],
    starBloom: 0.14,
  },
  'portable-chrome': {
    // The four PlayStation face-button hues (--portable-* tokens), Nova's
    // ambient accents for this theme.
    nebulae: () => [[90, 147, 214], [181, 131, 181], [212, 131, 138], [111, 191, 138]],
    nebulaOpacity: [0.022, 0.048],
    nebulaRadius: [120, 280],
    galaxyRadius: [60, 130],
    galaxyGlow: [0.03, 0.06],
    galaxyCore: [0.05, 0.1],
    galaxyRing: [0.08, 0.15],
    starBloom: 0.1,
  },
  'high-contrast': {
    nebulae: (glow) => [glow, brighten(glow, 0.4)],
    nebulaOpacity: [0.008, 0.018],
    nebulaRadius: [120, 260],
    galaxyRadius: [56, 120],
    galaxyGlow: [0.02, 0.05],
    galaxyCore: [0.04, 0.08],
    galaxyRing: [0.06, 0.12],
    starBloom: 0.08,
  },
}

function getThemePalette(theme) {
  const tokens = readThemeTokens(['ice', 'accent', 'accent-2'])
  const star = hexTriplet(tokens.ice, [200, 214, 229])
  const glow = hexTriplet(tokens.accent, [124, 115, 255])
  const accent2 = hexTriplet(tokens['accent-2'], glow)
  const art = THEME_ART[theme] || THEME_ART.polaris
  return {
    starRgb: star,
    glowRgb: glow,
    shootingRgb: brighten(star, 0.45),
    headRgb: brighten(star, 0.75),
    nebulae: art.nebulae(glow, accent2),
    nebulaOpacity: art.nebulaOpacity,
    nebulaRadius: art.nebulaRadius,
    galaxyRadius: art.galaxyRadius,
    galaxyGlow: art.galaxyGlow,
    galaxyCore: art.galaxyCore,
    galaxyRing: art.galaxyRing,
    starBloom: art.starBloom,
  }
}

function mixStar(tint) {
  return currentPalette.starRgb.map((channel, i) => Math.round(channel * 0.8 + tint[i] * 0.2))
}

function createParticle() {
  const isBright = Math.random() < currentConfig.brightChance
  return {
    x: Math.random() * w,
    y: Math.random() * h,
    size: isBright
      ? randomBetween(currentConfig.brightSize[0], currentConfig.brightSize[1])
      : randomBetween(currentConfig.dimSize[0], currentConfig.dimSize[1]),
    speedX: (Math.random() - 0.5) * currentConfig.driftX,
    speedY: (Math.random() - 0.5) * currentConfig.driftY - 0.02,
    opacity: isBright
      ? randomBetween(currentConfig.brightOpacity[0], currentConfig.brightOpacity[1])
      : randomBetween(currentConfig.dimOpacity[0], currentConfig.dimOpacity[1]),
    // Warm/cool stars are a restrained mix of the theme's own primary text.
    color: [currentPalette.starRgb, mixStar([255, 211, 170]), mixStar([165, 195, 255])][Math.floor(Math.random() * 3)],
    sparkle: isBright && Math.random() < 0.16,
    twinkleSpeed: prefersReducedMotion ? 0 : randomBetween(currentConfig.twinkle[0], currentConfig.twinkle[1]),
    twinklePhase: Math.random() * Math.PI * 2,
    isBright,
  }
}

function createShootingStar() {
  const angle = Math.random() * 0.5 + 0.2
  return {
    x: Math.random() * w * 0.8,
    y: Math.random() * h * 0.4,
    speed: randomBetween(1.5, 2.5),
    angle,
    dx: Math.cos(angle),
    dy: Math.sin(angle),
    life: 0,
    maxLife: Math.random() * 40 + 20,
    size: Math.random() * 1.5 + 0.5,
  }
}

function createNebula() {
  return {
    x: Math.random() * w,
    y: Math.random() * h,
    radius: randomBetween(currentPalette.nebulaRadius[0], currentPalette.nebulaRadius[1]),
    color: currentPalette.nebulae[Math.floor(Math.random() * currentPalette.nebulae.length)],
    opacity: randomBetween(currentPalette.nebulaOpacity[0], currentPalette.nebulaOpacity[1]),
    squash: randomBetween(0.45, 0.85),
    rotation: Math.random() * Math.PI,
  }
}

function createGalaxyObject() {
  const radius = randomBetween(currentPalette.galaxyRadius[0], currentPalette.galaxyRadius[1])
  return {
    x: Math.random() * w,
    y: Math.random() * h,
    radius,
    squash: randomBetween(0.42, 0.76),
    rotation: Math.random() * Math.PI,
    spiral: Math.random() < 0.6,
    glowOpacity: randomBetween(currentPalette.galaxyGlow[0], currentPalette.galaxyGlow[1]),
    coreOpacity: randomBetween(currentPalette.galaxyCore[0], currentPalette.galaxyCore[1]),
    ringOpacity: randomBetween(currentPalette.galaxyRing[0], currentPalette.galaxyRing[1]),
    color: currentPalette.nebulae[Math.floor(Math.random() * currentPalette.nebulae.length)],
  }
}

// Every mode keeps its full star set; the surface area decides how many draw.
function activeParticleCount() {
  const scaled = Math.round(currentConfig.count * (w * h) / REFERENCE_AREA)
  return Math.max(Math.round(currentConfig.count * MIN_DENSITY_SHARE), Math.min(currentConfig.count, scaled))
}

function rebuild() {
  particles = Array.from({ length: currentConfig.count }, () => createParticle())
  nebulae = Array.from({ length: currentConfig.nebulaCount }, createNebula)
  galaxyObjects = Array.from({ length: currentConfig.galaxyCount }, createGalaxyObject)
  shootingStars = []
  paintBackdrop()
}

function syncVisualMode() {
  currentMode = resolveMode()
  currentTheme = getTheme()
  currentConfig = getModeConfig(currentMode)
  currentPalette = getThemePalette(currentTheme)
  if (canvas.value) {
    const themeAlpha = getThemeParticleAlpha(currentTheme)
    canvas.value.style.opacity = String(themeAlpha)
    canvas.value.style.display = themeAlpha === 0 ? 'none' : ''
  }
}

function syncReducedMotionPreference() {
  prefersReducedMotion = !!motionMediaQuery?.matches
}

// Size events only measure and keep the CSS box exact; the pane canvas is also
// clamped to its pane, so a size still in flight never widens the layout. The
// backing store, backdrop and draw wait for one animation frame.
function measure() {
  if (props.absolute && canvas.value.parentElement) {
    pendingW = Math.max(1, canvas.value.parentElement.clientWidth)
    pendingH = Math.max(1, canvas.value.parentElement.scrollHeight || canvas.value.parentElement.clientHeight)
  } else {
    pendingW = Math.max(1, window.innerWidth)
    pendingH = Math.max(1, window.innerHeight)
  }
  canvas.value.style.width = `${pendingW}px`
  canvas.value.style.height = `${pendingH}px`
  resizePending = true
}

// Applies a measured size to the main backing store. Returns the previous size
// when anything changed, otherwise null.
function applyCanvasSize() {
  if (!resizePending || !canvas.value || !ctx) return null
  resizePending = false
  const ratio = Math.min(window.devicePixelRatio || 1, MAX_DPR,
    Math.sqrt(MAX_PIXELS / (pendingW * pendingH)), MAX_SIDE / pendingW, MAX_SIDE / pendingH)
  const width = Math.max(1, Math.floor(pendingW * ratio))
  const height = Math.max(1, Math.floor(pendingH * ratio))
  if (pendingW === w && pendingH === h && ratio === pixelRatio &&
    canvas.value.width === width && canvas.value.height === height) return null
  const previous = { w, h }
  w = pendingW
  h = pendingH
  pixelRatio = ratio
  canvas.value.width = width
  canvas.value.height = height
  ctx.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0)
  return previous
}

// A resize keeps every star, nebula and galaxy. Positions scale with the surface,
// so the field stretches instead of reshuffling, and only the drawn share of the
// star set follows the new area.
function applyPendingResize() {
  const previous = applyCanvasSize()
  if (!previous) return false
  const sx = w / previous.w
  const sy = h / previous.h
  for (const list of [particles, nebulae, galaxyObjects, shootingStars]) {
    for (const item of list) {
      item.x *= sx
      item.y *= sy
    }
  }
  paintBackdrop()
  return true
}

function scheduleResize() {
  if (!canvas.value || !ctx) return
  measure()
  // Hidden tabs and particle-free themes apply the size when they draw again.
  if (!pageVisible || getThemeParticleAlpha(currentTheme) === 0) return
  // A running loop applies it on its next frame; otherwise one frame is queued.
  if (animId !== null || resizeFrame !== null) return
  resizeFrame = requestAnimationFrame(flushResize)
}

// The window resize event and the pane observer can both report one change; only
// a size that differs from the last applied one draws again.
function flushResize() {
  resizeFrame = null
  if (animId !== null || !applyPendingResize()) return
  restartAnimation()
}

// Ambient layers are static depth, cached on resize/theme/mode changes. Each
// frame blits one image rather than rebuilding large translucent gradients.
function paintBackdrop() {
  if (!backdropCtx || !canvas.value) return
  backdrop.width = canvas.value.width
  backdrop.height = canvas.value.height
  const c = backdropCtx
  c.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0)
  c.clearRect(0, 0, w, h)
  for (const n of nebulae) {
    c.save()
    c.translate(n.x, n.y)
    c.rotate(n.rotation)
    c.scale(1, n.squash)
    const grad = c.createRadialGradient(0, 0, 0, 0, 0, n.radius)
    grad.addColorStop(0, `rgba(${n.color.join(',')}, ${n.opacity})`)
    grad.addColorStop(0.4, `rgba(${n.color.join(',')}, ${n.opacity * 0.5})`)
    grad.addColorStop(1, 'rgba(0, 0, 0, 0)')
    c.fillStyle = grad
    c.fillRect(-n.radius, -n.radius, n.radius * 2, n.radius * 2)
    c.restore()
  }
  for (const galaxy of galaxyObjects) {
    c.save()
    c.translate(galaxy.x, galaxy.y)
    c.rotate(galaxy.rotation)
    c.scale(1, galaxy.squash)
    const grad = c.createRadialGradient(0, 0, 0, 0, 0, galaxy.radius)
    grad.addColorStop(0, `rgba(${currentPalette.headRgb.join(',')}, ${galaxy.coreOpacity})`)
    grad.addColorStop(0.12, `rgba(${currentPalette.glowRgb.join(',')}, ${galaxy.glowOpacity})`)
    grad.addColorStop(0.45, `rgba(${galaxy.color.join(',')}, ${galaxy.glowOpacity * 0.4})`)
    grad.addColorStop(1, 'rgba(0, 0, 0, 0)')
    c.fillStyle = grad
    c.beginPath()
    c.arc(0, 0, galaxy.radius, 0, Math.PI * 2)
    c.fill()
    // Sparse, faint spiral arms or an elliptical halo; no bright orbit rings.
    if (galaxy.spiral) {
      c.strokeStyle = `rgba(${galaxy.color.join(',')}, ${galaxy.ringOpacity * 0.35})`
      c.lineWidth = 1
      for (let arm = 0; arm < 2; arm++) {
        c.beginPath()
        for (let i = 0; i <= 24; i++) {
          const t = i / 24
          const angle = t * Math.PI * 1.6 + arm * Math.PI
          const radius = galaxy.radius * (0.08 + t * 0.62)
          const x = Math.cos(angle) * radius, y = Math.sin(angle) * radius
          if (i === 0) c.moveTo(x, y)
          else c.lineTo(x, y)
        }
        c.stroke()
      }
    }
    c.restore()
  }
}

function animate(timestamp = 0, staticFrame = false) {
  animId = null
  if (!ctx || !pageVisible || getThemeParticleAlpha(currentTheme) === 0) return
  // A resized surface draws on this frame rather than waiting out the 30 Hz gate.
  const resized = applyPendingResize()
  if (!resized && !staticFrame && lastFrameTime !== null && timestamp - lastFrameTime < FRAME_INTERVAL) {
    animId = requestAnimationFrame(animate)
    return
  }
  // Capped elapsed time prevents a catch-up jump after a stalled/hidden tab.
  const step = staticFrame || lastFrameTime === null ? 0 : Math.min(50, timestamp - lastFrameTime) / (1000 / 60)
  lastFrameTime = timestamp
  ctx.clearRect(0, 0, w, h)
  if (backdropCtx) ctx.drawImage(backdrop, 0, 0, w, h)

  // Stars
  const activeCount = activeParticleCount()
  for (let i = 0; i < activeCount; i++) {
    const p = particles[i]
    if (!staticFrame) {
      p.x += p.speedX * step
      p.y += p.speedY * step
    }

    if (p.x < -5) p.x = w + 5
    if (p.x > w + 5) p.x = -5
    if (p.y < -5) p.y = h + 5
    if (p.y > h + 5) p.y = -5

    if (!staticFrame) {
      p.twinklePhase += p.twinkleSpeed * step
    }
    const twinkle = staticFrame ? 1 : Math.sin(p.twinklePhase) * 0.3 + 0.7
    const alpha = p.opacity * twinkle

    ctx.beginPath()
    ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2)
    ctx.fillStyle = `rgba(${p.color.join(',')}, ${alpha})`
    ctx.fill()

    if (p.isBright && alpha > 0.3) {
      ctx.beginPath()
      ctx.arc(p.x, p.y, p.size * 3, 0, Math.PI * 2)
      ctx.fillStyle = `rgba(${currentPalette.glowRgb[0]}, ${currentPalette.glowRgb[1]}, ${currentPalette.glowRgb[2]}, ${alpha * currentPalette.starBloom})`
      ctx.fill()

      if (p.sparkle && p.size > 1.3 && twinkle > currentConfig.sparkleThreshold) {
        ctx.strokeStyle = `rgba(${currentPalette.starRgb[0]}, ${currentPalette.starRgb[1]}, ${currentPalette.starRgb[2]}, ${alpha * 0.32})`
        ctx.lineWidth = 0.5
        const len = p.size * 4
        ctx.beginPath()
        ctx.moveTo(p.x - len, p.y)
        ctx.lineTo(p.x + len, p.y)
        ctx.moveTo(p.x, p.y - len)
        ctx.lineTo(p.x, p.y + len)
        ctx.stroke()
      }
    }
  }

  if (!staticFrame && Math.random() < 1 - (1 - currentConfig.shootingChance) ** step && shootingStars.length < currentConfig.maxShootingStars) {
    shootingStars.push(createShootingStar())
  }

  for (let i = shootingStars.length - 1; i >= 0; i--) {
    const s = shootingStars[i]
    if (!staticFrame) {
      s.x += s.dx * s.speed * step
      s.y += s.dy * s.speed * step
      s.life += step
    }

    const progress = s.life / s.maxLife
    const fade = progress < 0.3 ? progress / 0.3 : 1 - ((progress - 0.3) / 0.7)
    const alpha = fade * 0.8

    ctx.beginPath()
    ctx.moveTo(s.x, s.y)
    ctx.lineTo(s.x - s.dx * 20, s.y - s.dy * 20)
    const trailGrad = ctx.createLinearGradient(s.x, s.y, s.x - s.dx * 20, s.y - s.dy * 20)
    trailGrad.addColorStop(0, `rgba(${currentPalette.shootingRgb[0]}, ${currentPalette.shootingRgb[1]}, ${currentPalette.shootingRgb[2]}, ${alpha})`)
    trailGrad.addColorStop(1, `rgba(${currentPalette.shootingRgb[0]}, ${currentPalette.shootingRgb[1]}, ${currentPalette.shootingRgb[2]}, 0)`)
    ctx.strokeStyle = trailGrad
    ctx.lineWidth = s.size
    ctx.stroke()

    ctx.beginPath()
    ctx.arc(s.x, s.y, s.size, 0, Math.PI * 2)
    ctx.fillStyle = `rgba(${currentPalette.headRgb[0]}, ${currentPalette.headRgb[1]}, ${currentPalette.headRgb[2]}, ${alpha})`
    ctx.fill()

    if (!staticFrame && (s.life >= s.maxLife || s.x > w + 50 || s.y > h + 50)) {
      shootingStars.splice(i, 1)
    }
  }

  if (!staticFrame) {
    animId = requestAnimationFrame(animate)
  }
}

function restartAnimation() {
  if (animId !== null) {
    cancelAnimationFrame(animId)
    animId = null
  }
  // The frame drawn below applies any pending size itself.
  if (resizeFrame !== null) {
    cancelAnimationFrame(resizeFrame)
    resizeFrame = null
  }
  lastFrameTime = null
  if (!pageVisible) return
  if (getThemeParticleAlpha(currentTheme) === 0) return
  animate(0, prefersReducedMotion)
}

function handleVisibilityChange() {
  pageVisible = typeof document === 'undefined' || !document.hidden
  if (!pageVisible) {
    if (animId !== null) {
      cancelAnimationFrame(animId)
      animId = null
    }
    // A size queued while visible waits too; it is applied when the tab returns.
    if (resizeFrame !== null) {
      cancelAnimationFrame(resizeFrame)
      resizeFrame = null
    }
    return
  }
  restartAnimation()
}

// Mode, theme and motion changes recreate the field, so a pending size is applied
// first and the new field is built at the surface it will draw on.
function rebuildField() {
  syncVisualMode()
  applyCanvasSize()
  rebuild()
  restartAnimation()
}

watch(() => [props.dense, props.mode], rebuildField)

// The field re-skins itself on theme swaps; no prop threading needed.
let unsubscribeTheme = null

onMounted(() => {
  if (!canvas.value) return
  ctx = canvas.value.getContext('2d')
  if (!ctx) return
  backdrop = document.createElement('canvas')
  backdropCtx = backdrop.getContext('2d')
  pageVisible = typeof document === 'undefined' || !document.hidden
  motionMediaQuery = window.matchMedia('(prefers-reduced-motion: reduce)')
  syncReducedMotionPreference()
  syncVisualMode()
  measure()
  applyCanvasSize()
  rebuild()
  window.addEventListener('resize', scheduleResize)
  document.addEventListener('visibilitychange', handleVisibilityChange)
  motionMediaQuery.addEventListener('change', handleReducedMotionChange)
  if (props.absolute && canvas.value.parentElement && typeof ResizeObserver !== 'undefined') {
    resizeObserver = new ResizeObserver(() => scheduleResize())
    resizeObserver.observe(canvas.value.parentElement)
  }
  unsubscribeTheme = onThemeChange(rebuildField)
  restartAnimation()
})

onUnmounted(() => {
  if (unsubscribeTheme) unsubscribeTheme()
  if (animId !== null) cancelAnimationFrame(animId)
  animId = null
  if (resizeFrame !== null) cancelAnimationFrame(resizeFrame)
  resizeFrame = null
  backdrop = null
  backdropCtx = null
  ctx = null
  window.removeEventListener('resize', scheduleResize)
  document.removeEventListener('visibilitychange', handleVisibilityChange)
  motionMediaQuery?.removeEventListener('change', handleReducedMotionChange)
  if (resizeObserver) resizeObserver.disconnect()
})

function handleReducedMotionChange() {
  syncReducedMotionPreference()
  rebuildField()
}
</script>
