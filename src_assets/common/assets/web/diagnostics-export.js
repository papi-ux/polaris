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

function lower(value) {
  return String(value || '').toLowerCase()
}

function linuxGpuProfile(stats = {}) {
  return stats?.linux_gpu_profile || stats?.linuxGpuProfile || {}
}

function linuxConfigurationWarnings(stats = {}) {
  const profile = linuxGpuProfile(stats)
  if (Array.isArray(profile.configuration_warnings)) return profile.configuration_warnings
  if (Array.isArray(profile.configurationWarnings)) return profile.configurationWarnings
  return []
}

function hostConfigurationWarningItem(stats = {}) {
  const warning = linuxConfigurationWarnings(stats)[0]
  if (!warning) return null

  const severity = lower(warning.severity)
  const status = severity === 'fail' || severity === 'error' ? 'fail' : 'warning'
  const detail = warning.message || warning.detail || 'Linux host configuration warning.'
  const action = warning.action || 'Review Linux streaming configuration before changing capture or encoder settings.'

  return checklistItem(
    'host-config',
    'Host configuration',
    status,
    redactSensitiveText(detail),
    redactSensitiveText(action)
  )
}

export function describeLinuxGpuProfile(stats = {}) {
  const profile = linuxGpuProfile(stats)
  const encoderApi = lower(profile.encoder_api || profile.encoderApi || stats?.encode_target_device)
  const captureReason = lower(stats?.capture_path_reason || stats?.capture?.reason)
  const capturePath = lower(stats?.capture_path || stats?.capture?.path || stats?.capture_transport)
  const gpuNativeRequested = Boolean(profile.gpu_native_requested ?? profile.gpuNativeRequested)
  const gpuNativeSucceeded = Boolean(
    profile.gpu_native_succeeded ??
    profile.gpuNativeSucceeded ??
    stats?.capture_gpu_native ??
    stats?.capture?.gpu_native
  )
  const adapterMatchesCaptureDevice = profile.adapter_matches_capture_device ?? profile.adapterMatchesCaptureDevice
  const cpuCopy = Boolean(
    stats?.capture_cpu_copy ||
    stats?.capture?.cpu_copy ||
    capturePath.includes('shm') ||
    captureReason.includes('shm')
  )

  if (encoderApi !== 'vaapi') return ''

  if (adapterMatchesCaptureDevice === false) {
    return 'AMD/VAAPI is active, but the capture render node does not match the encoder adapter. Review the /dev/dri/renderD* selection before blaming the client.'
  }

  if (cpuCopy) {
    if (gpuNativeRequested && !gpuNativeSucceeded) {
      return 'AMD/VAAPI is active, but GPU-native capture fell back to SHM/system-memory frames. This can be a safe conservative Private Stream baseline.'
    }
    return 'AMD/VAAPI is active with SHM/system-memory capture. This can be a safe conservative Private Stream baseline.'
  }

  if (gpuNativeSucceeded || stats?.capture_gpu_native || stats?.capture?.gpu_native) {
    return 'AMD/VAAPI is using GPU-native capture; capture and encode are staying on the GPU path.'
  }

  return 'AMD/VAAPI is active. Compare the reported capture path, render node, and encoder adapter before changing advanced capture flags.'
}

export function buildFixMyStreamChecklist({ stats = {}, statsConnected = false, logs = '', recentIssues = [] } = {}) {
  const streaming = Boolean(stats?.streaming)
  const packetLoss = Number(stats?.packet_loss)
  const encodeTime = Number(stats?.encode_time_ms)
  const captureKnown = Boolean(stats?.capture_path || stats?.capture_transport || stats?.capture_path_reason)
  const captureCpuCopy = Boolean(stats?.capture_cpu_copy)
  const gpuProfileDescription = describeLinuxGpuProfile(stats)
  const authPairingIssue = latestIssueMatching(logs, ['auth', 'pair', 'pin', 'credential', 'unauthorized', 'forbidden'])
  const recentIssueCount = Array.isArray(recentIssues) ? recentIssues.length : 0

  const connection = !statsConnected
    ? checklistItem('connection', 'Connection', 'warning', 'Stream telemetry is disconnected, so Polaris cannot confirm the live session path yet.', 'Refresh the page, verify the host is reachable, then start or resume the stream.')
    : streaming
      ? checklistItem('connection', 'Connection', 'pass', 'Live telemetry is connected and a stream is active.', 'Keep this page open while reproducing the issue.')
      : checklistItem('connection', 'Connection', 'warning', 'Telemetry is connected, but no active stream is running.', 'Start the affected game/session before exporting diagnostics.')
  const hostConfig = hostConfigurationWarningItem(stats)

  const loss = Number.isFinite(packetLoss)
    ? packetLoss > 2
      ? checklistItem('packet-loss', 'Packet loss', 'fail', `Packet loss is ${packetLoss.toFixed(1)}%, which can look like stutter before the encoder is at fault.`, 'Try wired/5 GHz, lower bitrate, or enable FEC before changing encoder settings.')
      : packetLoss > 0.5
        ? checklistItem('packet-loss', 'Packet loss', 'warning', `Packet loss is ${packetLoss.toFixed(1)}%; watch for artifacts and input delay.`, 'Lower bitrate one step and re-test the same scene.')
        : checklistItem('packet-loss', 'Packet loss', 'pass', `Packet loss is ${packetLoss.toFixed(1)}%.`, 'Network is not the loudest signal right now.')
    : checklistItem('packet-loss', 'Packet loss', 'warning', 'Packet loss has not been reported yet.', 'Start a live stream and wait for session telemetry.')

  const capture = captureCpuCopy
    ? checklistItem(
      'capture-path',
      'Capture path',
      'fail',
      gpuProfileDescription || 'The active capture path is crossing SHM/system-memory frames.',
      gpuProfileDescription
        ? 'Treat the conservative Private Stream baseline as valid, then review render-node pairing and support evidence before testing GPU-native or KMS/DRM capture.'
        : 'On Linux, compare DMA-BUF/GPU-native capture telemetry with the selected display and encoder adapter before changing advanced capture settings.'
    )
    : stats?.capture_gpu_native
      ? checklistItem('capture-path', 'Capture path', 'pass', gpuProfileDescription || 'Capture is reported as GPU-native.', 'Keep display pairing as-is unless the stream feels wrong.')
      : captureKnown
        ? checklistItem('capture-path', 'Capture path', 'warning', gpuProfileDescription || `Capture path is ${stats.capture_path || stats.capture_transport || 'mixed/unknown'}.`, 'Check whether the chosen display and encoder are paired to the intended GPU path.')
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

  return [
    connection,
    ...(hostConfig ? [hostConfig] : []),
    loss,
    capture,
    encoder,
    authPairing,
    logsItem,
  ]
}

function firstNonEmpty(...values) {
  for (const value of values) {
    if (value !== null && value !== undefined && String(value).trim() !== '') return value
  }
  return ''
}

function formatIssueValue(value, fallback = 'unknown') {
  const safe = sanitizeDiagnosticsValue(value)
  if (safe === null || safe === undefined || safe === '') return fallback
  if (typeof safe === 'object') return JSON.stringify(safe)
  return String(safe)
}

function formatIssueNumber(value, digits = 1, fallback = 'unknown') {
  const numeric = Number(value)
  return Number.isFinite(numeric) ? numeric.toFixed(digits) : fallback
}

function issueDraftLine(label, value, fallback = 'unknown') {
  return `- ${label}: ${formatIssueValue(value, fallback)}`
}

function summarizeActiveStream(stats = {}) {
  if (!stats || Object.keys(stats).length === 0) return 'unknown'
  const streaming = stats.streaming ? 'yes' : 'no'
  const fps = formatIssueNumber(stats.fps, 1)
  const target = formatIssueNumber(stats.session_target_fps || stats.requested_client_fps, 1)
  const bitrate = formatIssueValue(stats.bitrate_kbps ?? stats.bitrate, 'unknown')
  const loss = formatIssueNumber(stats.packet_loss, 2)
  const encode = formatIssueNumber(stats.encode_time_ms, 1)
  return `${streaming}, ${fps} FPS / ${target} target, ${bitrate} kbps, ${loss}% loss, ${encode} ms encode`
}

function formatRecentIssues(recentIssues = []) {
  if (!Array.isArray(recentIssues) || recentIssues.length === 0) return '- No recent warnings/errors were included.'
  return recentIssues.slice(0, 12).map((entry) => {
    const prefix = [entry.timestamp, entry.level].filter(Boolean).join(' ')
    const count = entry.count > 1 ? ` (${entry.count}x)` : ''
    return `- ${redactSensitiveText(`${prefix}: ${entry.message || entry.detail || ''}${count}`.trim())}`
  }).join('\n')
}

function formatChecklist(checklist = []) {
  if (!Array.isArray(checklist) || checklist.length === 0) return '- No Fix My Stream checklist was included.'
  return checklist.map((item) => `- ${formatIssueValue(item.label, 'Check')}: ${formatIssueValue(item.status, 'unknown')} — ${formatIssueValue(item.detail, '')}${item.action ? ` Next: ${formatIssueValue(item.action, '')}` : ''}`).join('\n')
}

function formatDoctorEvidence(doctor = {}) {
  if (!doctor || !Array.isArray(doctor.evidence) || doctor.evidence.length === 0) return '- No Doctor evidence was included.'
  return doctor.evidence.slice(0, 8).map((entry) => `- ${formatIssueValue(entry.id || entry.label, 'evidence')}: ${formatIssueValue(entry.detail || entry.value || entry.reason, '')}`).join('\n')
}

export function buildStreamEvidence(input = {}) {
  const stats = input.session_snapshot || input.stream_stats || {}
  const doctor = stats?.doctor || input.doctor || {}
  return sanitizeDiagnosticsValue({
    client: input.client || { type: stats.client_type || stats.client_name || 'unknown', name: stats.client_name },
    launch_mode: stats.launch_mode || stats.stream_display_mode || stats.runtime_backend || input.launch_mode,
    encoder: stats.encoder || stats.encode_target_device || input.encoder,
    capture_path: stats.capture_path || stats.capture_transport,
    capture_path_reason: stats.capture_path_reason,
    active_stream: summarizeActiveStream(stats),
    doctor,
    fix_my_stream_checklist: input.fix_my_stream_checklist || [],
    recent_issues: input.recent_issues || [],
  })
}

export function buildGithubIssueDraft(input = {}) {
  const safeInput = sanitizeDiagnosticsValue(input)
  const stats = safeInput.session_snapshot || safeInput.stream_stats || {}
  const config = safeInput.config || {}
  const system = safeInput.system_stats || {}
  const doctor = stats.doctor || safeInput.doctor || {}
  const client = safeInput.client || {}
  const gpu = firstNonEmpty(system?.gpu?.name, system?.gpu_name, config.gpu, stats.gpu_name, stats.encode_target_device)
  const driver = firstNonEmpty(system?.gpu?.driver, system?.driver, config.driver, stats.driver)
  const distro = firstNonEmpty(system?.os?.distro, system?.distro, config.distro, safeInput.platform)
  const sessionType = firstNonEmpty(system?.session?.type, system?.session_type, config.session_type, stats.session_type)
  const compositor = firstNonEmpty(system?.session?.compositor, system?.compositor, config.compositor, stats.compositor)
  const clientType = firstNonEmpty(client.type, stats.client_type, stats.client_name, 'unknown')
  const clientName = firstNonEmpty(client.name, stats.client_name)
  const capture = `${formatIssueValue(stats.capture_path || stats.capture_transport, 'unknown')} — ${formatIssueValue(stats.capture_path_reason, 'unknown')}`
  const doctorSummary = firstNonEmpty(doctor.simple_state, doctor.summary, doctor.diagnosis, doctor.primary_issue, 'Polaris did not include a Doctor diagnosis yet.')

  return redactSensitiveText([
    '# Polaris support report',
    '',
    '> This report was generated locally from a redacted Polaris support bundle. Nothing was submitted automatically.',
    '',
    '## Environment',
    issueDraftLine('Polaris version', safeInput.version),
    issueDraftLine('Distro', distro),
    issueDraftLine('GPU', gpu),
    issueDraftLine('Driver', driver),
    issueDraftLine('Session/compositor', `${formatIssueValue(sessionType)} / ${formatIssueValue(compositor)}`),
    issueDraftLine('Client', `${formatIssueValue(clientType)}${clientName ? ` (${formatIssueValue(clientName)})` : ''}`),
    '',
    '## Stream evidence',
    issueDraftLine('Launch mode', firstNonEmpty(stats.launch_mode, stats.stream_display_mode, stats.runtime_backend)),
    issueDraftLine('Encoder', firstNonEmpty(stats.encoder, stats.encode_target_device, config.encoder)),
    issueDraftLine('Capture', capture),
    issueDraftLine('Active stream', summarizeActiveStream(stats)),
    '',
    '## What Polaris thinks happened',
    formatIssueValue(doctorSummary, 'No Doctor summary was included.'),
    doctor.primary_issue ? `
Primary issue: ${formatIssueValue(doctor.primary_issue)}` : '',
    doctor.safe_recovery_action?.id ? `
Suggested safe action: ${formatIssueValue(doctor.safe_recovery_action.id)}${doctor.safe_recovery_action.destructive ? ' (destructive)' : ' (non-destructive)'}` : '',
    '',
    '## Fix My Stream checklist',
    formatChecklist(safeInput.fix_my_stream_checklist),
    '',
    '## Doctor evidence',
    formatDoctorEvidence(doctor),
    '',
    '## Recent warnings/errors',
    formatRecentIssues(safeInput.recent_issues),
    '',
    '## What I already tried',
    formatIssueValue(safeInput.user_notes, 'Not provided yet.'),
  ].filter((line) => line !== '').join('\n'))
}

export function buildAnonymizedDiagnosticsBundle(input = {}) {
  const streamEvidence = input.stream_evidence || buildStreamEvidence(input)
  const issueDraft = input.issue_draft || buildGithubIssueDraft({ ...input, stream_evidence: streamEvidence })
  return sanitizeDiagnosticsValue({
    generated_at: input.generated_at || new Date().toISOString(),
    export_kind: 'polaris-anonymized-diagnostics',
    redaction_notice: 'Fields containing password, token, secret, key, cookie, auth, or credential are redacted client-side before export.',
    support_bundle_version: 2,
    ...input,
    stream_evidence: streamEvidence,
    issue_draft: issueDraft,
  })
}
