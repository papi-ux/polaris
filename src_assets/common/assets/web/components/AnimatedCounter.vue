<template>
  <canvas ref="canvas" :width="canvasWidth" :height="canvasHeight" class="block"></canvas>
</template>

<script setup>
import { ref, watch, onMounted, onUnmounted } from 'vue'
import { prepare, layout } from '@chenglou/pretext'

const props = defineProps({
  value: { type: Number, default: 0 },
  suffix: { type: String, default: '' },
  decimals: { type: Number, default: 1 },
  color: { type: String, default: '#c8d6e5' },
  fontSize: { type: Number, default: 36 },
  font: { type: String, default: 'Inter, system-ui, sans-serif' }
})

const canvas = ref(null)
const canvasWidth = ref(160)
const canvasHeight = ref(48)
let currentValue = 0
let targetValue = 0
let animationFrame = null

const fontStr = () => `bold ${props.fontSize}px ${props.font}`

function formatValue(v) {
  return v.toFixed(props.decimals) + props.suffix
}

function sizeCanvasToFit() {
  // Use pretext to pre-measure the maximum expected width.
  // We measure a wide sample string so the canvas never clips.
  const sampleTexts = ['999.9' + props.suffix, '8888.8' + props.suffix, formatValue(props.value)]
  let maxWidth = 0
  for (const t of sampleTexts) {
    const prepared = prepare(t, fontStr())
    layout(prepared, 9999, props.fontSize * 1.2)
    // result.height gives single-line height; for width we can use a secondary
    // layout at 0 maxWidth to force one-char-per-line, but that would be wrong.
    // Instead, layout at a huge maxWidth guarantees 1 line, so height = lineHeight.
    // For width, we measure via canvas as pretext doesn't expose single-line width
    // directly from the opaque PreparedText. Use canvas measureText as fallback.
    const ctx = canvas.value?.getContext('2d')
    if (ctx) {
      ctx.font = fontStr()
      const m = ctx.measureText(t)
      if (m.width > maxWidth) maxWidth = m.width
    }
  }
  // Add a small margin
  canvasWidth.value = Math.ceil(maxWidth + 4)
  canvasHeight.value = Math.ceil(props.fontSize * 1.4)
}

function draw() {
  const ctx = canvas.value?.getContext('2d')
  if (!ctx) return

  // Smooth interpolation toward target
  currentValue += (targetValue - currentValue) * 0.15
  if (Math.abs(currentValue - targetValue) < 0.01) {
    currentValue = targetValue
  }

  const text = formatValue(currentValue)

  // Use pretext to verify layout fits in a single line at current canvas width.
  // This is a cheap arithmetic-only call (~0.0002ms).
  const prepared = prepare(text, fontStr())
  const textLayout = layout(prepared, canvasWidth.value, props.fontSize * 1.2)

  // If pretext says we'd need more than 1 line, grow the canvas
  if (textLayout.lineCount > 1) {
    sizeCanvasToFit()
  }

  ctx.clearRect(0, 0, canvasWidth.value, canvasHeight.value)
  ctx.font = fontStr()
  ctx.fillStyle = props.color
  ctx.textBaseline = 'middle'
  ctx.fillText(text, 0, canvasHeight.value / 2)

  if (currentValue !== targetValue) {
    animationFrame = requestAnimationFrame(draw)
  } else {
    animationFrame = null
  }
}

watch(() => props.value, (newVal) => {
  targetValue = newVal
  if (!animationFrame) {
    animationFrame = requestAnimationFrame(draw)
  }
})

watch(() => props.suffix, () => {
  sizeCanvasToFit()
  if (!animationFrame) {
    animationFrame = requestAnimationFrame(draw)
  }
})

onMounted(() => {
  targetValue = props.value
  currentValue = props.value
  sizeCanvasToFit()
  draw()
})

onUnmounted(() => {
  if (animationFrame) {
    cancelAnimationFrame(animationFrame)
    animationFrame = null
  }
})
</script>
