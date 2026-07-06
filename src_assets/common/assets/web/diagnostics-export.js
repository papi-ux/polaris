const SENSITIVE_FIELD_PATTERN = /(password|token|secret|key|cookie|auth|credential)/i
const SENSITIVE_ASSIGNMENT_PATTERN = /\b(password|token|secret|key|cookie|auth|credential)(\s*[:=]\s*)("[^"]*"|'[^']*'|[^\s,;})\]]+)/gi
const AUTH_HEADER_PATTERN = /\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]+/gi
const COOKIE_HEADER_PATTERN = /\b(cookie|set-cookie)(\s*[:=]\s*)[^\n;]+/gi
const URL_CREDENTIAL_PATTERN = /(https?:\/\/)([^\s/@:]+):([^\s/@]+)@/gi

export const REDACTED_VALUE = '[redacted]'

export function isSensitiveFieldName(key) {
  return SENSITIVE_FIELD_PATTERN.test(String(key || ''))
}

export function redactSensitiveText(value) {
  return String(value || '')
    .replace(URL_CREDENTIAL_PATTERN, `$1${REDACTED_VALUE}:${REDACTED_VALUE}@`)
    .replace(AUTH_HEADER_PATTERN, (_, scheme) => `${scheme} ${REDACTED_VALUE}`)
    .replace(COOKIE_HEADER_PATTERN, (_, label, separator) => `${label}${separator}${REDACTED_VALUE}`)
    .replace(SENSITIVE_ASSIGNMENT_PATTERN, (_, label, separator) => `${label}${separator}${REDACTED_VALUE}`)
}

export function sanitizeDiagnosticsValue(value, seen = new WeakSet()) {
  if (value === null || value === undefined) return value
  if (typeof value === 'string') return redactSensitiveText(value)
  if (typeof value !== 'object') return value
  if (seen.has(value)) return '[circular]'
  seen.add(value)

  if (Array.isArray(value)) {
    return value.map((item) => sanitizeDiagnosticsValue(item, seen))
  }

  return Object.fromEntries(Object.entries(value).map(([key, item]) => [
    key,
    isSensitiveFieldName(key) ? REDACTED_VALUE : sanitizeDiagnosticsValue(item, seen),
  ]))
}

function issueTermPattern(term) {
  const normalized = String(term || '').toLowerCase()
  const tokenBoundary = (pattern) => new RegExp(`(?:^|[^a-z0-9])${pattern}(?=$|[^a-z0-9])`, 'i')

  if (normalized === 'auth') return tokenBoundary('auth(?:entication|orization)?')
  if (normalized === 'pair') return tokenBoundary('pair(?:ed|ing)?')
  if (normalized === 'pin') return tokenBoundary('pin')

  const escaped = normalized.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  return tokenBoundary(escaped)
}

function latestIssueMatching(logs = '', terms = []) {
  const patterns = terms.map(issueTermPattern)
  return String(logs || '')
    .split('\n')
    .reverse()
    .find((line) => {
      return /(error|warning|fatal)/i.test(line) && patterns.some((pattern) => pattern.test(line))
    })
}

function checklistItem(key, label, status, detail, action) {
  return { key, label, status, detail, action }
}

export function buildFixMyStreamChecklist({ stats = {}, statsConnected = false, logs = '', recentIssues = [] } = {}) {
  const streaming = Boolean(stats?.streaming)
  const packetLoss = Number(stats?.packet_loss)
  const encodeTime = Number(stats?.encode_time_ms)
  const captureKnown = Boolean(stats?.capture_path || stats?.capture_transport || stats?.capture_path_reason)
  const captureCpuCopy = Boolean(stats?.capture_cpu_copy)
  const authPairingIssue = latestIssueMatching(logs, ['auth', 'pair', 'pin', 'credential', 'unauthorized', 'forbidden'])
  const recentIssueCount = Array.isArray(recentIssues) ? recentIssues.length : 0

  const connection = !statsConnected
    ? checklistItem('connection', 'Connection', 'warning', 'Stream telemetry is disconnected, so Polaris cannot confirm the live session path yet.', 'Refresh the page, verify the host is reachable, then start or resume the stream.')
    : streaming
      ? checklistItem('connection', 'Connection', 'pass', 'Live telemetry is connected and a stream is active.', 'Keep this page open while reproducing the issue.')
      : checklistItem('connection', 'Connection', 'warning', 'Telemetry is connected, but no active stream is running.', 'Start the affected game/session before exporting diagnostics.')

  const loss = Number.isFinite(packetLoss)
    ? packetLoss > 2
      ? checklistItem('packet-loss', 'Packet loss', 'fail', `Packet loss is ${packetLoss.toFixed(1)}%, which can look like stutter before the encoder is at fault.`, 'Try wired/5 GHz, lower bitrate, or enable FEC before changing encoder settings.')
      : packetLoss > 0.5
        ? checklistItem('packet-loss', 'Packet loss', 'warning', `Packet loss is ${packetLoss.toFixed(1)}%; watch for artifacts and input delay.`, 'Lower bitrate one step and re-test the same scene.')
        : checklistItem('packet-loss', 'Packet loss', 'pass', `Packet loss is ${packetLoss.toFixed(1)}%.`, 'Network is not the loudest signal right now.')
    : checklistItem('packet-loss', 'Packet loss', 'warning', 'Packet loss has not been reported yet.', 'Start a live stream and wait for session telemetry.')

  const capture = captureCpuCopy
    ? checklistItem('capture-path', 'Capture path', 'fail', 'The active capture path is crossing system memory/CPU copy.', 'On Linux, prefer DMA-BUF/GPU-native capture for high-FPS streams; check Audio/Video display pairing and capture settings.')
    : stats?.capture_gpu_native
      ? checklistItem('capture-path', 'Capture path', 'pass', 'Capture is reported as GPU-native.', 'Keep display pairing as-is unless the stream feels wrong.')
      : captureKnown
        ? checklistItem('capture-path', 'Capture path', 'warning', `Capture path is ${stats.capture_path || stats.capture_transport || 'mixed/unknown'}.`, 'Check whether the chosen display and encoder are paired to the intended GPU path.')
        : checklistItem('capture-path', 'Capture path', 'warning', 'No capture metadata has been reported yet.', 'Start a stream, then confirm capture path and display pairing.')

  const encoder = Number.isFinite(encodeTime)
    ? encodeTime > 12
      ? checklistItem('encoder-pressure', 'Encoder pressure', 'fail', `${encodeTime.toFixed(1)} ms encode time is over the safe low-latency budget.`, 'Lower resolution/FPS, lower bitrate, or switch to the low-latency hardware preset.')
      : encodeTime > 8
        ? checklistItem('encoder-pressure', 'Encoder pressure', 'warning', `${encodeTime.toFixed(1)} ms encode time is close to the 120 FPS budget.`, 'Watch for frame pacing spikes before chasing network fixes.')
        : checklistItem('encoder-pressure', 'Encoder pressure', 'pass', `${encodeTime.toFixed(1)} ms encode time leaves headroom.`, 'Encoder pressure is not the loudest signal right now.')
    : checklistItem('encoder-pressure', 'Encoder pressure', 'warning', 'Encoder timing has not been reported yet.', 'Start a live stream and wait for encoder telemetry.')

  const authPairing = authPairingIssue
    ? checklistItem('auth-pairing', 'Auth / pairing', 'fail', redactSensitiveText(authPairingIssue), 'Re-pair the client or verify Web UI credentials/trust before tuning stream quality.')
    : checklistItem('auth-pairing', 'Auth / pairing', 'pass', 'No recent auth or pairing errors stand out in the current log window.', 'If the client cannot connect, export diagnostics right after the failed pairing attempt.')

  const logsItem = recentIssueCount > 0
    ? checklistItem('logs', 'Logs', 'warning', `${recentIssueCount} recent warning/error group${recentIssueCount === 1 ? '' : 's'} are visible.`, 'Copy recent issues or export anonymized diagnostics for support.')
    : checklistItem('logs', 'Logs', 'pass', 'No recent warnings, fatals, or errors are standing out.', 'Keep the live log open while reproducing the issue.')

  return [connection, loss, capture, encoder, authPairing, logsItem]
}

export function buildAnonymizedDiagnosticsBundle(input = {}) {
  return sanitizeDiagnosticsValue({
    generated_at: input.generated_at || new Date().toISOString(),
    export_kind: 'polaris-anonymized-diagnostics',
    redaction_notice: 'Fields containing password, token, secret, key, cookie, auth, or credential are redacted client-side before export.',
    ...input,
  })
}
