export const DISPLAY_PLANNER_SCALE_FACTORS = [0.5, 0.75, 1, 1.25, 1.5, 2]

export const DISPLAY_PLANNER_PRESETS = [
  {
    id: 'native',
    title: 'Native',
    intent: 'Match the client panel exactly.',
    scaleFactor: 1,
  },
  {
    id: 'balanced',
    title: 'Balanced',
    intent: 'Best for this device: preserve aspect ratio while easing encoder and network load.',
    scaleFactor: 0.75,
  },
  {
    id: 'sharp',
    title: 'Sharp / Supersampled',
    intent: 'Render above the client panel and downscale for extra clarity when the host has headroom.',
    scaleFactor: 1.5,
    advanced: true,
  },
  {
    id: 'performance',
    title: 'Performance',
    intent: 'Favor frame pacing and bandwidth over raw pixel count.',
    scaleFactor: 0.5,
  },
  {
    id: 'custom',
    title: 'Custom',
    intent: 'Advanced manual scale factor using the existing fallback display mode field.',
    custom: true,
    advanced: true,
  },
]

const DEFAULT_DEVICE = Object.freeze({ width: 1920, height: 1080, fps: 60 })
const DEFAULT_LIMITS = Object.freeze({ maxWidth: 7680, maxHeight: 4320, maxPixels: 7680 * 4320 })

export function parseDisplayMode(value, fallback = DEFAULT_DEVICE) {
  const match = String(value || '').trim().match(/^(\d+)x(\d+)x(\d+(?:\.\d+)?)$/)
  if (!match) return { ...fallback }

  return {
    width: Number(match[1]),
    height: Number(match[2]),
    fps: Number(match[3]),
  }
}

export function formatDisplayMode({ width, height, fps = 60 }) {
  return `${Math.round(width)}x${Math.round(height)}x${Number(fps)}`
}

export function clampPlannerScale(value) {
  const numeric = Number(value)
  if (!Number.isFinite(numeric)) return 1
  return Math.min(2, Math.max(0.5, numeric))
}

export function buildResolutionPlanner({
  fallbackMode = '',
  device = null,
  customScale = 1,
  showAdvanced = false,
  limits = DEFAULT_LIMITS,
} = {}) {
  const base = normalizeDevice(device || parseDisplayMode(fallbackMode))
  const effectiveLimits = { ...DEFAULT_LIMITS, ...(limits || {}) }
  const customFactor = clampPlannerScale(customScale)

  const choices = DISPLAY_PLANNER_PRESETS.map((preset) => {
    const scaleFactor = preset.custom ? customFactor : preset.scaleFactor
    const target = resolveScaledMode(base, scaleFactor)
    const safe = isSafeMode(target, effectiveLimits)
    const hidden = Boolean((preset.advanced && !showAdvanced) || !safe)
    const reason = safe
      ? preset.intent
      : 'Hidden because this would exceed the safe display-mode envelope for normal users.'

    return {
      ...preset,
      scaleFactor,
      target,
      targetMode: formatDisplayMode(target),
      hidden,
      safe,
      reason,
      badge: resolvePresetBadge(preset.id, base, scaleFactor),
      aspectRatio: aspectRatioLabel(base.width, base.height),
    }
  })

  const visibleChoices = choices.filter((choice) => !choice.hidden)
  const recommended = visibleChoices.find((choice) => choice.id === 'balanced') || visibleChoices[0] || choices[0]

  return {
    sourceMode: formatDisplayMode(base),
    sourceAspectRatio: aspectRatioLabel(base.width, base.height),
    sourceFps: base.fps,
    recommendedId: recommended.id,
    recommendedTitle: 'Best for this device',
    recommendedMode: recommended.targetMode,
    choices,
    visibleChoices,
    advancedScaleFactors: DISPLAY_PLANNER_SCALE_FACTORS.map((scaleFactor) => {
      const target = resolveScaledMode(base, scaleFactor)
      return {
        scaleFactor,
        label: `${scaleFactor}x`,
        target,
        targetMode: formatDisplayMode(target),
        safe: isSafeMode(target, effectiveLimits),
      }
    }),
  }
}

export function resolveScaledMode(base, scaleFactor) {
  const scale = clampPlannerScale(scaleFactor)
  const width = roundToEven(base.width * scale)
  const height = roundToEven(base.height * scale)
  return { width, height, fps: base.fps }
}

function normalizeDevice(device = {}) {
  const width = positiveNumber(device.width, DEFAULT_DEVICE.width)
  const height = positiveNumber(device.height, DEFAULT_DEVICE.height)
  const fps = positiveNumber(device.fps, DEFAULT_DEVICE.fps)
  return { width, height, fps }
}

function positiveNumber(value, fallback) {
  const numeric = Number(value)
  return Number.isFinite(numeric) && numeric > 0 ? numeric : fallback
}

function roundToEven(value) {
  const rounded = Math.max(2, Math.round(value))
  return rounded % 2 === 0 ? rounded : rounded + 1
}

function isSafeMode(target, limits) {
  return target.width <= limits.maxWidth
    && target.height <= limits.maxHeight
    && target.width * target.height <= limits.maxPixels
}

function resolvePresetBadge(id, base, scaleFactor) {
  if (id === 'balanced') return 'Best for this device'
  if (id === 'native') return `${base.width}×${base.height}`
  if (id === 'sharp') return `${scaleFactor}x supersample`
  if (id === 'performance') return `${scaleFactor}x downscale`
  return 'Advanced'
}

function aspectRatioLabel(width, height) {
  const divisor = gcd(width, height)
  return `${width / divisor}:${height / divisor}`
}

function gcd(a, b) {
  let x = Math.abs(Math.round(a))
  let y = Math.abs(Math.round(b))
  while (y) {
    const next = x % y
    x = y
    y = next
  }
  return x || 1
}
