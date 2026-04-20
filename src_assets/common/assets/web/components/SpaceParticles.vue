<template>
  <canvas ref="canvas" class="space-particles inset-0 pointer-events-none" :class="absolute ? 'absolute' : 'fixed'" :style="{ zIndex: absolute ? 0 : 0 }" />
</template>

<script setup>
import { ref, watch, onMounted, onUnmounted } from 'vue'

const props = defineProps({
  dense: { type: Boolean, default: false },
  absolute: { type: Boolean, default: false },
  mode: { type: String, default: '' },
  theme: { type: String, default: 'polaris' },
})

const canvas = ref(null)
let ctx = null
let animId = null
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
let motionMediaQuery = null
let prefersReducedMotion = false

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
      shootingChance: 0.01,
      maxShootingStars: 5,
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
      shootingChance: 0.008,
      maxShootingStars: 4,
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
    shootingChance: 0.005,
    maxShootingStars: 3,
    sparkleThreshold: 0.8,
  }
}

function getThemePalette(theme) {
  if (theme === 'oled') {
    return {
      starRgb: [245, 249, 255],
      glowRgb: [120, 188, 255],
      nebulae: [
        [40, 90, 196],
        [62, 124, 255],
        [23, 63, 150],
        [158, 219, 255],
      ],
      nebulaOpacity: [0.016, 0.036],
      nebulaRadius: [150, 360],
      galaxyRadius: [78, 180],
      galaxyGlow: [0.05, 0.1],
      galaxyCore: [0.09, 0.18],
      galaxyRing: [0.14, 0.26],
      starBloom: 0.16,
      shootingRgb: [242, 248, 255],
      headRgb: [255, 255, 255],
    }
  }

  return {
    starRgb: [208, 220, 238],
    glowRgb: [124, 115, 255],
    nebulae: [
      [124, 115, 255],
      [98, 132, 190],
      [88, 116, 146],
    ],
    nebulaOpacity: [0.012, 0.028],
    nebulaRadius: [120, 300],
    galaxyRadius: [66, 150],
    galaxyGlow: [0.04, 0.08],
    galaxyCore: [0.07, 0.14],
    galaxyRing: [0.11, 0.2],
    starBloom: 0.12,
    shootingRgb: [226, 234, 248],
    headRgb: [240, 244, 255],
  }
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
    speed: Math.random() * 4 + 3,
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
    drift: prefersReducedMotion ? 0 : (Math.random() - 0.5) * 0.02,
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
    driftX: prefersReducedMotion ? 0 : (Math.random() - 0.5) * 0.018,
    driftY: prefersReducedMotion ? 0 : (Math.random() - 0.5) * 0.012,
    glowOpacity: randomBetween(currentPalette.galaxyGlow[0], currentPalette.galaxyGlow[1]),
    coreOpacity: randomBetween(currentPalette.galaxyCore[0], currentPalette.galaxyCore[1]),
    ringOpacity: randomBetween(currentPalette.galaxyRing[0], currentPalette.galaxyRing[1]),
    color: currentPalette.nebulae[Math.floor(Math.random() * currentPalette.nebulae.length)],
  }
}

function rebuild() {
  particles = Array.from({ length: currentConfig.count }, () => createParticle())
  nebulae = Array.from({ length: currentConfig.nebulaCount }, createNebula)
  galaxyObjects = Array.from({ length: currentConfig.galaxyCount }, createGalaxyObject)
  shootingStars = []
}

function syncVisualMode() {
  currentMode = resolveMode()
  currentTheme = props.theme || 'polaris'
  currentConfig = getModeConfig(currentMode)
  currentPalette = getThemePalette(currentTheme)
}

function syncReducedMotionPreference() {
  prefersReducedMotion = !!motionMediaQuery?.matches
}

function resize() {
  if (!canvas.value) return
  if (props.absolute && canvas.value.parentElement) {
    w = canvas.value.parentElement.clientWidth
    h = canvas.value.parentElement.scrollHeight || canvas.value.parentElement.clientHeight
  } else {
    w = window.innerWidth
    h = window.innerHeight
  }
  const pixelRatio = window.devicePixelRatio || 1
  canvas.value.width = Math.max(1, Math.floor(w * pixelRatio))
  canvas.value.height = Math.max(1, Math.floor(h * pixelRatio))
  canvas.value.style.width = `${w}px`
  canvas.value.style.height = `${h}px`
  if (ctx) {
    ctx.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0)
  }
  if (prefersReducedMotion && ctx) {
    restartAnimation()
  }
}

function animate(staticFrame = false) {
  if (!ctx) return
  ctx.clearRect(0, 0, w, h)

  for (const n of nebulae) {
    if (!staticFrame) {
      n.x += n.drift
    }
    const grad = ctx.createRadialGradient(n.x, n.y, 0, n.x, n.y, n.radius)
    grad.addColorStop(0, `rgba(${n.color[0]}, ${n.color[1]}, ${n.color[2]}, ${n.opacity})`)
    grad.addColorStop(1, 'rgba(0, 0, 0, 0)')
    ctx.fillStyle = grad
    ctx.fillRect(n.x - n.radius, n.y - n.radius, n.radius * 2, n.radius * 2)
  }

  for (const galaxy of galaxyObjects) {
    if (!staticFrame) {
      galaxy.x += galaxy.driftX
      galaxy.y += galaxy.driftY
    }

    if (galaxy.x < -galaxy.radius) galaxy.x = w + galaxy.radius
    if (galaxy.x > w + galaxy.radius) galaxy.x = -galaxy.radius
    if (galaxy.y < -galaxy.radius) galaxy.y = h + galaxy.radius
    if (galaxy.y > h + galaxy.radius) galaxy.y = -galaxy.radius

    ctx.save()
    ctx.translate(galaxy.x, galaxy.y)
    ctx.rotate(galaxy.rotation)
    ctx.scale(1, galaxy.squash)

    const grad = ctx.createRadialGradient(0, 0, 0, 0, 0, galaxy.radius)
    grad.addColorStop(0, `rgba(${currentPalette.glowRgb[0]}, ${currentPalette.glowRgb[1]}, ${currentPalette.glowRgb[2]}, ${galaxy.coreOpacity})`)
    grad.addColorStop(0.35, `rgba(${galaxy.color[0]}, ${galaxy.color[1]}, ${galaxy.color[2]}, ${galaxy.glowOpacity})`)
    grad.addColorStop(1, 'rgba(0, 0, 0, 0)')
    ctx.fillStyle = grad
    ctx.beginPath()
    ctx.arc(0, 0, galaxy.radius, 0, Math.PI * 2)
    ctx.fill()

    ctx.fillStyle = `rgba(${currentPalette.headRgb[0]}, ${currentPalette.headRgb[1]}, ${currentPalette.headRgb[2]}, ${galaxy.coreOpacity * 0.95})`
    ctx.beginPath()
    ctx.arc(0, 0, galaxy.radius * 0.08, 0, Math.PI * 2)
    ctx.fill()

    ctx.strokeStyle = `rgba(${currentPalette.starRgb[0]}, ${currentPalette.starRgb[1]}, ${currentPalette.starRgb[2]}, ${galaxy.ringOpacity})`
    ctx.lineWidth = 0.75
    ctx.beginPath()
    ctx.ellipse(0, 0, galaxy.radius * 0.7, galaxy.radius * 0.2, 0, 0, Math.PI * 2)
    ctx.stroke()

    ctx.strokeStyle = `rgba(${currentPalette.glowRgb[0]}, ${currentPalette.glowRgb[1]}, ${currentPalette.glowRgb[2]}, ${galaxy.ringOpacity * 0.5})`
    ctx.lineWidth = 0.5
    ctx.beginPath()
    ctx.ellipse(0, 0, galaxy.radius * 0.48, galaxy.radius * 0.14, 0, 0, Math.PI * 2)
    ctx.stroke()

    ctx.restore()
  }

  // Stars
  for (const p of particles) {
    if (!staticFrame) {
      p.x += p.speedX
      p.y += p.speedY
    }

    if (p.x < -5) p.x = w + 5
    if (p.x > w + 5) p.x = -5
    if (p.y < -5) p.y = h + 5
    if (p.y > h + 5) p.y = -5

    if (!staticFrame) {
      p.twinklePhase += p.twinkleSpeed
    }
    const twinkle = staticFrame ? 1 : Math.sin(p.twinklePhase) * 0.3 + 0.7
    const alpha = p.opacity * twinkle

    ctx.beginPath()
    ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2)
    ctx.fillStyle = `rgba(${currentPalette.starRgb[0]}, ${currentPalette.starRgb[1]}, ${currentPalette.starRgb[2]}, ${alpha})`
    ctx.fill()

    if (p.isBright && alpha > 0.3) {
      ctx.beginPath()
      ctx.arc(p.x, p.y, p.size * 3, 0, Math.PI * 2)
      ctx.fillStyle = `rgba(${currentPalette.glowRgb[0]}, ${currentPalette.glowRgb[1]}, ${currentPalette.glowRgb[2]}, ${alpha * currentPalette.starBloom})`
      ctx.fill()

      if (p.size > 1.3 && twinkle > currentConfig.sparkleThreshold) {
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

  if (!staticFrame && Math.random() < currentConfig.shootingChance && shootingStars.length < currentConfig.maxShootingStars) {
    shootingStars.push(createShootingStar())
  }

  for (let i = shootingStars.length - 1; i >= 0; i--) {
    const s = shootingStars[i]
    if (!staticFrame) {
      s.x += s.dx * s.speed
      s.y += s.dy * s.speed
      s.life++
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
    animId = requestAnimationFrame(() => animate(false))
  }
}

function restartAnimation() {
  if (animId) {
    cancelAnimationFrame(animId)
    animId = null
  }
  animate(prefersReducedMotion)
}

watch(() => [props.dense, props.mode, props.theme], () => {
  syncVisualMode()
  rebuild()
  restartAnimation()
})

onMounted(() => {
  if (!canvas.value) return
  ctx = canvas.value.getContext('2d')
  motionMediaQuery = window.matchMedia('(prefers-reduced-motion: reduce)')
  syncReducedMotionPreference()
  syncVisualMode()
  resize()
  rebuild()
  window.addEventListener('resize', resize)
  motionMediaQuery.addEventListener('change', handleReducedMotionChange)
  if (props.absolute && canvas.value.parentElement && typeof ResizeObserver !== 'undefined') {
    resizeObserver = new ResizeObserver(() => resize())
    resizeObserver.observe(canvas.value.parentElement)
  }
  restartAnimation()
})

onUnmounted(() => {
  if (animId) cancelAnimationFrame(animId)
  window.removeEventListener('resize', resize)
  motionMediaQuery?.removeEventListener('change', handleReducedMotionChange)
  if (resizeObserver) resizeObserver.disconnect()
})

function handleReducedMotionChange() {
  syncReducedMotionPreference()
  rebuild()
  restartAnimation()
}
</script>
