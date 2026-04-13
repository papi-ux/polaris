<template>
  <canvas ref="canvas" class="inset-0 pointer-events-none" :class="absolute ? 'absolute' : 'fixed'" :style="{ zIndex: absolute ? 0 : 0 }" />
</template>

<script setup>
import { ref, watch, onMounted, onUnmounted } from 'vue'

const props = defineProps({
  dense: { type: Boolean, default: false },
  absolute: { type: Boolean, default: false }
})

const canvas = ref(null)
let ctx = null
let animId = null
let particles = []
let shootingStars = []
let nebulae = []
let w = 0
let h = 0
let currentDense = false

function createParticle(dense) {
  const isBright = Math.random() < (dense ? 0.15 : 0.1)
  return {
    x: Math.random() * w,
    y: Math.random() * h,
    size: dense
      ? (isBright ? Math.random() * 2.5 + 1 : Math.random() * 1.5 + 0.3)
      : (isBright ? Math.random() * 2 + 0.8 : Math.random() * 1.5 + 0.5),
    speedX: (Math.random() - 0.5) * (dense ? 0.08 : 0.12),
    speedY: (Math.random() - 0.5) * 0.1 - (dense ? 0.02 : 0.03),
    opacity: dense
      ? (isBright ? Math.random() * 0.7 + 0.3 : Math.random() * 0.4 + 0.05)
      : (isBright ? Math.random() * 0.6 + 0.3 : Math.random() * 0.4 + 0.15),
    twinkleSpeed: Math.random() * (dense ? 0.008 : 0.005) + 0.002,
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
    radius: Math.random() * 200 + 100,
    color: Math.random() < 0.5
      ? [100, 120, 180]   // blue nebula
      : [140, 100, 160],  // purple nebula
    opacity: Math.random() * 0.03 + 0.01,
    drift: (Math.random() - 0.5) * 0.02,
  }
}

function rebuild() {
  const dense = currentDense
  const count = dense ? 300 : 120
  particles = Array.from({ length: count }, () => createParticle(dense))
  if (dense) {
    nebulae = Array.from({ length: 6 }, createNebula)
    shootingStars = []
  } else {
    nebulae = []
    shootingStars = []
  }
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
  canvas.value.width = w
  canvas.value.height = h
}

function animate() {
  if (!ctx) return
  ctx.clearRect(0, 0, w, h)
  const dense = currentDense

  // Nebulae (dense mode only)
  for (const n of nebulae) {
    n.x += n.drift
    const grad = ctx.createRadialGradient(n.x, n.y, 0, n.x, n.y, n.radius)
    grad.addColorStop(0, `rgba(${n.color[0]}, ${n.color[1]}, ${n.color[2]}, ${n.opacity})`)
    grad.addColorStop(1, 'rgba(0, 0, 0, 0)')
    ctx.fillStyle = grad
    ctx.fillRect(n.x - n.radius, n.y - n.radius, n.radius * 2, n.radius * 2)
  }

  // Stars
  for (const p of particles) {
    p.x += p.speedX
    p.y += p.speedY

    if (p.x < -5) p.x = w + 5
    if (p.x > w + 5) p.x = -5
    if (p.y < -5) p.y = h + 5
    if (p.y > h + 5) p.y = -5

    p.twinklePhase += p.twinkleSpeed
    const twinkle = Math.sin(p.twinklePhase) * 0.3 + 0.7
    const alpha = p.opacity * twinkle

    // Star core
    ctx.beginPath()
    ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2)
    ctx.fillStyle = `rgba(200, 214, 229, ${alpha})`
    ctx.fill()

    // Glow for bright stars
    if (p.isBright && alpha > 0.3) {
      ctx.beginPath()
      ctx.arc(p.x, p.y, p.size * 3, 0, Math.PI * 2)
      ctx.fillStyle = `rgba(200, 214, 229, ${alpha * 0.08})`
      ctx.fill()

      // Cross sparkle for brightest
      if (dense && p.size > 1.5 && twinkle > 0.85) {
        ctx.strokeStyle = `rgba(200, 214, 229, ${alpha * 0.3})`
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

  // Shooting stars (dense mode, occasional)
  if (dense && Math.random() < 0.005 && shootingStars.length < 2) {
    shootingStars.push(createShootingStar())
  }

  for (let i = shootingStars.length - 1; i >= 0; i--) {
    const s = shootingStars[i]
    s.x += s.dx * s.speed
    s.y += s.dy * s.speed
    s.life++

    const progress = s.life / s.maxLife
    const fade = progress < 0.3 ? progress / 0.3 : 1 - ((progress - 0.3) / 0.7)
    const alpha = fade * 0.8

    // Trail
    ctx.beginPath()
    ctx.moveTo(s.x, s.y)
    ctx.lineTo(s.x - s.dx * 20, s.y - s.dy * 20)
    const trailGrad = ctx.createLinearGradient(s.x, s.y, s.x - s.dx * 20, s.y - s.dy * 20)
    trailGrad.addColorStop(0, `rgba(200, 214, 229, ${alpha})`)
    trailGrad.addColorStop(1, 'rgba(200, 214, 229, 0)')
    ctx.strokeStyle = trailGrad
    ctx.lineWidth = s.size
    ctx.stroke()

    // Head
    ctx.beginPath()
    ctx.arc(s.x, s.y, s.size, 0, Math.PI * 2)
    ctx.fillStyle = `rgba(220, 230, 245, ${alpha})`
    ctx.fill()

    if (s.life >= s.maxLife || s.x > w + 50 || s.y > h + 50) {
      shootingStars.splice(i, 1)
    }
  }

  animId = requestAnimationFrame(animate)
}

watch(() => props.dense, (val) => {
  currentDense = val
  rebuild()
})

onMounted(() => {
  if (!canvas.value) return
  ctx = canvas.value.getContext('2d')
  currentDense = props.dense
  resize()
  rebuild()
  window.addEventListener('resize', resize)
  animate()
})

onUnmounted(() => {
  if (animId) cancelAnimationFrame(animId)
  window.removeEventListener('resize', resize)
})
</script>
