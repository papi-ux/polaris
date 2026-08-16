const ENCODER_PROBE_START = 'Testing for available encoders, this may generate errors. You can safely ignore those errors.'
const ENCODER_PROBE_END = 'Ignore any errors mentioned above, they are not relevant.'

function probeBoundary(line) {
  if (line.includes(ENCODER_PROBE_START)) return 'start'
  if (line.includes(ENCODER_PROBE_END)) return 'end'
  return null
}

function parseIssueLine(line) {
  const match = line.match(/^(\[[^\]]+\]):\s*(Fatal|Warning|Error):\s*(.*)$/)
  if (!match) return null

  return {
    timestamp: match[1],
    level: match[2],
    message: match[3].replace(/\s+/g, ' ').trim(),
  }
}

/**
 * Group recent warning/error log lines while excluding capability-probe noise.
 *
 * Encoder probing intentionally tries unsupported codec/profile combinations.
 * Polaris brackets those attempts with stable start/end banners. Filtering by
 * that context keeps an identical codec failure visible when it occurs during
 * a real stream. If a bounded log tail begins inside a probe, the first end
 * banner also lets us infer that the leading lines belong to the probe window.
 */
export function groupRecentIssueLogs(logText, {
  maxSourceLines = 300,
  maxGroups = Number.POSITIVE_INFINITY,
} = {}) {
  const lines = String(logText || '').split('\n').slice(-maxSourceLines)
  const firstStart = lines.findIndex((line) => probeBoundary(line) === 'start')
  const firstEnd = lines.findIndex((line) => probeBoundary(line) === 'end')
  let insideEncoderProbe = firstEnd >= 0 && (firstStart < 0 || firstEnd < firstStart)
  const visibleIssues = []

  for (const line of lines) {
    const boundary = probeBoundary(line)
    if (boundary === 'start') {
      insideEncoderProbe = true
      continue
    }
    if (boundary === 'end') {
      insideEncoderProbe = false
      continue
    }

    const issue = parseIssueLine(line)
    if (issue && !insideEncoderProbe) visibleIssues.push(issue)
  }

  const grouped = new Map()
  visibleIssues.reverse().forEach((entry) => {
    const key = `${entry.level}:${entry.message}`
    if (!grouped.has(key)) {
      grouped.set(key, { ...entry, count: 1 })
      return
    }
    grouped.get(key).count += 1
  })

  return Array.from(grouped.values()).slice(0, maxGroups)
}
