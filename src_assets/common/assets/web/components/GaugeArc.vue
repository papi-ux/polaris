<template>
  <div class="flex flex-col items-center gap-1">
    <svg :width="size" :height="size * 0.65" :viewBox="`0 0 ${size} ${size * 0.65}`">
      <!-- Background arc -->
      <path :d="bgArc" fill="none" :stroke="trackColor" :stroke-width="strokeWidth" stroke-linecap="round" />
      <!-- Value arc -->
      <path :d="valueArc" fill="none" :stroke="currentColor" :stroke-width="strokeWidth" stroke-linecap="round"
            style="transition: d 0.5s ease, stroke 0.3s ease" />
      <!-- Value text -->
      <text :x="size / 2" :y="size * 0.48" text-anchor="middle" :fill="currentColor"
            :font-size="size * 0.22" font-weight="700" font-family="Inter, system-ui, sans-serif">
        {{ displayValue }}
      </text>
      <text :x="size / 2" :y="size * 0.62" text-anchor="middle" fill="#687b81"
            :font-size="size * 0.1" font-family="Inter, system-ui, sans-serif">
        {{ unit }}
      </text>
    </svg>
    <div class="text-xs text-storm text-center leading-tight">{{ label }}</div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

const props = defineProps({
  value: { type: Number, default: 0 },
  max: { type: Number, default: 100 },
  unit: { type: String, default: '%' },
  label: { type: String, default: '' },
  size: { type: Number, default: 100 },
  strokeWidth: { type: Number, default: 8 },
  thresholds: {
    type: Array,
    default: () => [
      { at: 0, color: '#22c55e' },    // green
      { at: 60, color: '#eab308' },   // yellow
      { at: 85, color: '#ef4444' },   // red
    ]
  }
})

const trackColor = 'rgba(104, 123, 129, 0.2)'

const pct = computed(() => Math.min(Math.max(props.value / props.max, 0), 1))

const currentColor = computed(() => {
  const p = pct.value * 100
  let color = props.thresholds[0]?.color || '#22c55e'
  for (const t of props.thresholds) {
    if (p >= t.at) color = t.color
  }
  return color
})

const displayValue = computed(() => {
  return Number.isInteger(props.value) ? props.value : props.value.toFixed(1)
})

// Arc geometry — 180° semicircle
const r = computed(() => (props.size - props.strokeWidth) / 2)
const cx = computed(() => props.size / 2)
const cy = computed(() => props.size * 0.52)

function describeArc(startAngle, endAngle) {
  const rad = Math.PI / 180
  const x1 = cx.value + r.value * Math.cos(rad * startAngle)
  const y1 = cy.value + r.value * Math.sin(rad * startAngle)
  const x2 = cx.value + r.value * Math.cos(rad * endAngle)
  const y2 = cy.value + r.value * Math.sin(rad * endAngle)
  const large = endAngle - startAngle > 180 ? 1 : 0
  return `M ${x1} ${y1} A ${r.value} ${r.value} 0 ${large} 1 ${x2} ${y2}`
}

const bgArc = computed(() => describeArc(180, 360))
const valueArc = computed(() => {
  const end = 180 + pct.value * 180
  return end <= 180.1 ? '' : describeArc(180, Math.min(end, 360))
})
</script>
