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
  const adapterPairingStatus = lower(profile.adapter_pairing_status || profile.adapterPairingStatus)
  const adapterPairingSource = lower(profile.adapter_pairing_device_source || profile.adapterPairingDeviceSource)
  const encoderAdapter = profile.encoder_adapter || profile.encoderAdapter || ''
  const captureDevice = profile.capture_device || profile.captureDevice || ''
  const waylandMainDevice = profile.wayland_main_device || profile.waylandMainDevice || ''
  const pairingDevice = profile.adapter_pairing_device || profile.adapterPairingDevice ||
    (adapterPairingSource === 'capture_device' ? captureDevice : adapterPairingSource === 'wayland_main_device' ? waylandMainDevice : captureDevice || waylandMainDevice)
  const pairingMismatch = Boolean(encoderAdapter && pairingDevice && (
    adapterPairingStatus ? adapterPairingStatus === 'mismatched' : adapterMatchesCaptureDevice === false
  ))
  const cpuCopy = Boolean(
    stats?.capture_cpu_copy ||
    stats?.capture?.cpu_copy ||
    capturePath.includes('shm') ||
    captureReason.includes('shm')
  )

  if (encoderApi !== 'vaapi') return ''

  if (pairingMismatch) {
    if (adapterPairingSource === 'wayland_main_device') {
      return `AMD/VAAPI is active, but encoder adapter ${encoderAdapter} does not match the Wayland compositor device ${waylandMainDevice}. Verify the /dev/dri/renderD* mapping before blaming the client.`
    }
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

function formatGpuNativeProbeAttempt(label, attempt) {
  if (!attempt || typeof attempt !== 'object' || Object.keys(attempt).length === 0) return ''
  const result = firstNonEmpty(attempt.result, 'unknown')
  const stage = firstNonEmpty(attempt.failure_stage, attempt.failureStage)
  const reason = firstNonEmpty(attempt.failure_reason, attempt.failureReason)
  const parts = [`result=${formatIssueValue(result)}`]
  const hasOwn = (key) => Object.prototype.hasOwnProperty.call(attempt, key)
  if (hasOwn('attempted')) parts.push(`attempted=${attempt.attempted ? 'yes' : 'no'}`)
  if (hasOwn('cached')) parts.push(`cached=${attempt.cached ? 'yes' : 'no'}`)
  if (stage) parts.push(`stage=${formatIssueValue(stage)}`)
  if (reason) parts.push(`reason=${formatIssueValue(reason)}`)
  return `${label}[${parts.join(', ')}]`
}

function formatGpuNativeProbe(probe = {}) {
  const attempts = [
    formatGpuNativeProbeAttempt('headless_extcopy', probe.headless_extcopy || probe.headlessExtcopy),
    formatGpuNativeProbeAttempt('windowed', probe.windowed),
  ].filter(Boolean)
  const outcomes = attempts.length > 0 ? attempts.join('; ') : 'outcomes unavailable'
  return `${outcomes} — selected ${formatIssueValue(probe.selected_strategy || probe.selectedStrategy, 'unknown')}, fallback ${formatIssueValue(probe.fallback, 'none')}`
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
  const gpuProfile = linuxGpuProfile(stats)
  const gpuNativeProbe = stats.gpu_native_probe || stats.gpuNativeProbe || {}
  const probeSummary = formatGpuNativeProbe(gpuNativeProbe)
  const gpuDiagnosticLines = []
  const encoderAdapter = firstNonEmpty(gpuProfile.encoder_adapter, gpuProfile.encoderAdapter)
  const captureDevice = firstNonEmpty(gpuProfile.capture_device, gpuProfile.captureDevice)
  const waylandMainDevice = firstNonEmpty(gpuProfile.wayland_main_device, gpuProfile.waylandMainDevice)
  const adapterPairing = firstNonEmpty(gpuProfile.adapter_pairing_status, gpuProfile.adapterPairingStatus)
  const adapterPairingSource = lower(gpuProfile.adapter_pairing_device_source || gpuProfile.adapterPairingDeviceSource)
  const adapterPairingDevice = firstNonEmpty(
    gpuProfile.adapter_pairing_device,
    gpuProfile.adapterPairingDevice,
    adapterPairingSource === 'capture_device' ? captureDevice : '',
    adapterPairingSource === 'wayland_main_device' ? waylandMainDevice : '',
    captureDevice,
    waylandMainDevice
  )
  if (encoderAdapter) gpuDiagnosticLines.push(issueDraftLine('Encoder adapter', encoderAdapter))
  if (captureDevice) gpuDiagnosticLines.push(issueDraftLine('Capture device', captureDevice))
  if (waylandMainDevice) gpuDiagnosticLines.push(issueDraftLine('Wayland main device', waylandMainDevice))
  if (encoderAdapter && adapterPairingDevice && adapterPairing) gpuDiagnosticLines.push(issueDraftLine('Adapter pairing', adapterPairing))
  if (Object.keys(gpuNativeProbe).length > 0) gpuDiagnosticLines.push(issueDraftLine('GPU-native probe', probeSummary))
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
    ...gpuDiagnosticLines,
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

function statusRank(status) {
  if (status === 'fail') return 2
  if (status === 'warning') return 1
  return 0
}

function worstStatus(items = []) {
  return items.reduce((worst, item) => statusRank(item.status) > statusRank(worst) ? item.status : worst, 'pass')
}

function average(values = []) {
  const numeric = values.map(Number).filter(Number.isFinite)
  if (!numeric.length) return null
  return numeric.reduce((sum, value) => sum + value, 0) / numeric.length
}

function jitter(values = []) {
  const numeric = values.map(Number).filter(Number.isFinite)
  if (numeric.length < 2) return null
  const deltas = numeric.slice(1).map((value, index) => Math.abs(value - numeric[index]))
  return average(deltas)
}

function isPrivateHost(host = '') {
  const value = String(host || '').toLowerCase()
  return value === 'localhost' ||
    value.endsWith('.local') ||
    value.startsWith('192.168.') ||
    value.startsWith('10.') ||
    /^172\.(1[6-9]|2\d|3[0-1])\./.test(value) ||
    value.startsWith('fd') ||
    value.startsWith('fe80:')
}

function reportLine(label, value) {
  return `${label}: ${value || '(unknown)'}`
}

function normalizeProbeStatus(status) {
  const value = lower(status)
  if (['open', 'reachable', 'pass', 'ok'].includes(value)) return 'pass'
  if (['closed', 'blocked', 'timeout', 'fail', 'failed', 'unreachable'].includes(value)) return 'fail'
  return 'warning'
}

function nativeProbePort(nativeProbe = {}, keyPattern) {
  const ports = Array.isArray(nativeProbe.ports) ? nativeProbe.ports : []
  return ports.find((port) => keyPattern.test(String(port.key || port.label || '')))
}

function nativeProbePortDetail(port) {
  if (!port) return ''
  const endpoint = [port.port, port.transport].filter(Boolean).join('/')
  const status = port.status || 'hint'
  const detail = port.detail || port.error || ''
  return `${port.label || port.key || 'Port'}${endpoint ? ` ${endpoint}` : ''}: ${status}${detail ? ` (${detail})` : ''}`
}

function nativeProbeSamples(nativeProbe = {}) {
  const samples = nativeProbe.samples || nativeProbe.latency || {}
  const latencySamples = Array.isArray(samples.latencyMs)
    ? samples.latencyMs
    : Array.isArray(samples.pingSamplesMs)
      ? samples.pingSamplesMs
      : Array.isArray(nativeProbe.pingSamplesMs)
        ? nativeProbe.pingSamplesMs
        : []
  return {
    latencySamples,
    jitterMs: samples.jitterMs ?? nativeProbe.jitterMs,
    packetLossPercent: samples.packetLossPercent ?? nativeProbe.packetLossPercent,
  }
}

function formatNativeProbeEvidence(nativeProbe = {}) {
  const ports = Array.isArray(nativeProbe.ports) ? nativeProbe.ports : []
  const lines = [
    `Native evidence: ${nativeProbe.classification || 'unknown'}${nativeProbe.targetHost ? ` path to ${nativeProbe.targetHost}` : ''}`,
    ...ports.map(nativeProbePortDetail),
    ...(Array.isArray(nativeProbe.notes) ? nativeProbe.notes : []),
  ].filter(Boolean)
  return redactSensitiveText(lines.join('\n'))
}

export function buildNetworkPathTestReport(input = {}) {
  const nativeProbe = input.nativeProbe && typeof input.nativeProbe === 'object' ? input.nativeProbe : null
  const nativeSamples = nativeProbe ? nativeProbeSamples(nativeProbe) : {}
  const samples = Array.isArray(nativeSamples.latencySamples) && nativeSamples.latencySamples.length
    ? nativeSamples.latencySamples.map(Number).filter(Number.isFinite)
    : Array.isArray(input.pingSamplesMs)
      ? input.pingSamplesMs.map(Number).filter(Number.isFinite)
      : []
  const latency = average(samples)
  const sampleJitter = Number.isFinite(Number(nativeSamples.jitterMs)) ? Number(nativeSamples.jitterMs) : jitter(samples)
  const loss = Number(nativeSamples.packetLossPercent ?? input.packetLossPercent ?? input.packet_loss ?? input.lossPercent)
  const host = nativeProbe?.targetHost || input.host || input.originHostname || input.hostname || ''
  const nativeClassification = lower(nativeProbe?.classification)
  const lanLike = nativeClassification ? ['pc', 'lan', 'local'].includes(nativeClassification) : isPrivateHost(host) || input.lan === true
  const vpnLike = nativeClassification === 'vpn'
  const pathLabel = lanLike ? 'LAN/local' : vpnLike ? 'VPN' : 'remote/VPN'
  const controlPort = nativeProbePort(nativeProbe || {}, /(control|https)/i)
  const streamPorts = Array.isArray(nativeProbe?.ports) ? nativeProbe.ports.filter((port) => /(stream|video|audio|rtsp|udp)/i.test(String(port.key || port.label || ''))) : []
  const streamPortFailed = streamPorts.some((port) => normalizeProbeStatus(port.status) === 'fail')
  const streamPortPassed = streamPorts.length > 0 && streamPorts.every((port) => normalizeProbeStatus(port.status) !== 'fail') && streamPorts.some((port) => normalizeProbeStatus(port.status) === 'pass')
  const currentBitrate = Number(input.currentBitrateKbps ?? input.bitrateKbps ?? input.bitrate_kbps)
  const latencyPenalty = Number.isFinite(latency) && latency > 60 ? 0.55 : Number.isFinite(latency) && latency > 30 ? 0.75 : 1
  const jitterPenalty = Number.isFinite(sampleJitter) && sampleJitter > 15 ? 0.7 : 1
  const lossPenalty = Number.isFinite(loss) && loss > 2 ? 0.45 : Number.isFinite(loss) && loss > 0.5 ? 0.7 : 1
  const fallbackBitrate = lanLike ? 50000 : 30000
  const bitrateBase = Number.isFinite(currentBitrate) && currentBitrate > 0 ? currentBitrate : fallbackBitrate
  const recommendedBitrateKbps = Math.max(8000, Math.min(bitrateBase, Math.round(bitrateBase * latencyPenalty * jitterPenalty * lossPenalty / 1000) * 1000))

  const checks = [
    checklistItem(
      'host-reachable',
      'Host reachable',
      nativeProbe?.hostReachable === false ? 'fail' : nativeProbe?.hostReachable === true ? 'pass' : input.hostReachable === false ? 'fail' : samples.length || input.hostReachable === true ? 'pass' : 'warning',
      samples.length ? `${samples.length} latency sample${samples.length === 1 ? '' : 's'} collected.` : nativeProbe ? 'Polaris server returned native reachability evidence for this Web UI request.' : 'Browser-side reachability is inferred from the Web UI session.',
      samples.length || nativeProbe ? 'Keep this host address for the client test.' : 'Open this page from the same client/network you plan to stream from.'
    ),
    checklistItem(
      'control-port',
      'Control port',
      controlPort ? normalizeProbeStatus(controlPort.status) : input.controlPortOpen === false ? 'fail' : input.controlPortOpen === true ? 'pass' : 'warning',
      controlPort ? nativeProbePortDetail(controlPort) : input.controlPortOpen === true ? 'Control/pairing path is reachable.' : input.controlPortOpen === false ? 'Control/pairing port did not respond.' : 'Control port was not directly tested by this browser.',
      (controlPort && normalizeProbeStatus(controlPort.status) === 'fail') || input.controlPortOpen === false ? 'Check host firewall/NAT before pairing.' : 'If pairing fails, verify the HTTPS/control port from the client network.'
    ),
    checklistItem(
      'stream-port',
      'Stream ports',
      streamPortFailed ? 'fail' : streamPortPassed ? 'pass' : input.streamPortOpen === false ? 'fail' : input.streamPortOpen === true ? 'pass' : 'warning',
      streamPorts.length ? streamPorts.map(nativeProbePortDetail).join(' · ') : input.streamPortOpen === true ? 'Stream ports look reachable.' : input.streamPortOpen === false ? 'One or more stream ports did not respond.' : 'Stream ports were not directly tested by this browser.',
      streamPortFailed || input.streamPortOpen === false ? 'Fix firewall/NAT before tuning bitrate.' : 'If video starts then freezes, retest UDP reachability from the client.'
    ),
    checklistItem(
      'discovery-mdns',
      'Discovery / mDNS',
      nativeProbe?.mdnsAvailable === false ? 'warning' : input.mdnsAvailable === false ? 'warning' : nativeProbe?.mdnsAvailable === true || input.mdnsAvailable === true || String(input.originHostname || '').endsWith('.local') ? 'pass' : 'warning',
      nativeProbe?.mdnsAvailable === false || input.mdnsAvailable === false ? 'mDNS discovery was not confirmed; manual host add may be needed.' : 'Discovery hints are present or not required.',
      nativeProbe?.mdnsAvailable === false || input.mdnsAvailable === false ? 'Use the host IP directly in Moonlight/Nova if auto-discovery misses it.' : 'Discovery is not the loudest signal right now.'
    ),
    checklistItem(
      'lan-vpn-clue',
      'LAN / VPN clue',
      lanLike || vpnLike ? 'pass' : 'warning',
      lanLike ? 'Host address looks LAN/local.' : vpnLike ? 'Host address looks VPN/overlay.' : 'Host address looks remote/VPN or public.',
      lanLike ? 'Start with normal LAN bitrate, then tune up.' : 'Expect lower bitrate and higher jitter until VPN/NAT path is proven stable.'
    ),
    checklistItem(
      'latency-jitter-loss',
      'Latency / jitter / loss',
      (Number.isFinite(loss) && loss > 2) || (Number.isFinite(sampleJitter) && sampleJitter > 20) ? 'fail' : (Number.isFinite(loss) && loss > 0.5) || (Number.isFinite(latency) && latency > 40) ? 'warning' : 'pass',
      `${Number.isFinite(latency) ? latency.toFixed(1) : 'unknown'} ms avg / ${Number.isFinite(sampleJitter) ? sampleJitter.toFixed(1) : 'unknown'} ms jitter / ${Number.isFinite(loss) ? loss.toFixed(1) : 'unknown'}% loss.`,
      (Number.isFinite(loss) && loss > 0.5) ? 'Lower bitrate one step and prefer wired/5 GHz before changing encoder settings.' : 'Network quality is not the loudest signal right now.'
    ),
    checklistItem(
      'bitrate-ceiling',
      'Recommended bitrate ceiling',
      recommendedBitrateKbps < bitrateBase ? 'warning' : 'pass',
      `Recommended ceiling: ${recommendedBitrateKbps} kbps.`,
      recommendedBitrateKbps < bitrateBase ? 'Use this as the next launch ceiling, then raise only after a clean session.' : 'Current bitrate target is reasonable for the sampled path.'
    ),
  ]

  const status = worstStatus(checks)
  return {
    kind: 'network-path-test',
    status,
    classification: status === 'pass' ? 'clean' : 'network',
    summary: `${pathLabel} path: ${status === 'fail' ? 'fix reachability/loss first' : status === 'warning' ? 'usable with caution' : 'ready'}.`,
    recommendedBitrateKbps,
    checks,
    advancedEvidence: sanitizeDiagnosticsValue({ host, samples, latency, jitter: sampleJitter, packetLossPercent: Number.isFinite(loss) ? loss : null, nativeProbe }),
    nativeEvidenceText: nativeProbe ? formatNativeProbeEvidence(nativeProbe) : '',
  }
}

export function buildControllerInputTestReport(input = {}) {
  const events = Array.isArray(input.events) ? input.events : []
  const gamepads = Array.isArray(input.gamepads) ? input.gamepads : []
  const native = input.native || input.controllerInput || input.controller_input || {}
  const virtual = input.virtualController || {
    created: native.virtualControllerCreated ?? native.virtual_controller_created,
    number: native.virtualControllerNumber ?? native.virtual_controller_number,
    kind: native.virtualControllerKind ?? native.virtual_controller_kind,
    error: native.virtualControllerError ?? native.virtual_controller_error,
  }
  const hostIsolationRaw = native.hostControllerIsolation ?? native.host_controller_isolation ?? input.hostPhysicalControllerIsolation
  const hostIsolation = lower(hostIsolationRaw)
  const hostIsolationDetail = native.hostControllerIsolationDetail ?? native.host_controller_isolation_detail
  const hapticsSupported = native.hapticsSupported ?? native.haptics_supported ?? input.rumbleSupported
  const hapticsDetail = native.hapticsDetail ?? native.haptics_detail
  const pads = new Set(events.map((event) => event.pad ?? event.gamepadIndex ?? 1))
  const visiblePadCount = Math.max(pads.size, gamepads.length)
  const virtualPadLabel = virtual.created
    ? `Native virtual controller${virtual.number ? ` #${virtual.number}` : ''}${virtual.kind ? ` (${virtual.kind})` : ''} is reported created.`
    : virtual.error
      ? redactSensitiveText(virtual.error)
      : 'Host virtual controller creation has not been confirmed yet.'
  const isolationPass = ['isolated', 'strict_bwrap', 'best_effort_sdl'].includes(hostIsolation)
  const isolationFail = ['shared', 'leaking', 'unavailable'].includes(hostIsolation)
  const checks = [
    checklistItem('client-events', 'Client button events', events.length ? 'pass' : 'warning', events.length ? `${events.length} client control event${events.length === 1 ? '' : 's'} detected.` : 'No client button or axis events detected yet.', events.length ? 'Input is reaching the browser/client layer.' : 'Press buttons/sticks on the client controller while this panel is open.'),
    checklistItem('virtual-controller', 'Native virtual controller', virtual.created ? 'pass' : virtual.error ? 'fail' : 'warning', virtualPadLabel, virtual.created ? 'Launch a game and verify the same controller number is selected.' : 'If games see no pad, check virtual gamepad permissions/driver state.'),
    checklistItem('multi-pad', 'Controller number / multi-pad', visiblePadCount > 1 ? 'pass' : 'warning', `${visiblePadCount} client pad${visiblePadCount === 1 ? '' : 's'} visible${virtual.number ? `; native virtual pad #${virtual.number}` : ''}.`, 'Keep controller order stable before starting split-screen or multi-pad games.'),
    checklistItem('rumble', 'Rumble / haptics', hapticsSupported === true ? 'pass' : 'warning', hapticsSupported === true ? (hapticsDetail || 'Rumble/haptics feedback is available for the native virtual controller.') : 'Rumble/haptics support is not available or not exposed by this browser/client.', hapticsSupported === true ? 'Use the optional rumble pulse only after input is mapped correctly.' : 'Treat missing rumble as non-blocking unless the game requires it.'),
    checklistItem('host-isolation', 'Host physical controller isolation', isolationPass ? 'pass' : isolationFail ? 'fail' : 'warning', isolationPass ? `Isolation state: ${hostIsolationRaw}${hostIsolationDetail ? ` — ${hostIsolationDetail}` : ''}.` : hostIsolation ? `Isolation state: ${hostIsolationRaw}${hostIsolationDetail ? ` — ${hostIsolationDetail}` : ''}.` : 'Host physical controller isolation has not been reported yet.', isolationPass ? 'No host-side controller conflict stands out.' : 'If inputs double-fire, unplug/disable the host physical controller or isolate it before retesting.'),
  ]
  const status = worstStatus(checks.filter((check) => check.key !== 'rumble'))
  return {
    kind: 'controller-input-test',
    status,
    classification: status === 'pass' ? 'clean' : 'client',
    summary: events.length ? `${events.length} client control event${events.length === 1 ? '' : 's'} captured; native virtual pad ${virtual.created ? `#${virtual.number || '?'}` : 'not confirmed'}.` : 'Waiting for client controller input.',
    checks,
    advancedEvidence: sanitizeDiagnosticsValue({ events: events.slice(-12), gamepads, native, virtualController: virtual, hostPhysicalControllerIsolation: hostIsolationRaw }),
  }
}

export function buildPostSessionStreamReport({ stats = {}, logs = '', disconnectReason = '' } = {}) {
  const loss = Number(stats.packet_loss)
  const latency = Number(stats.latency_ms)
  const encodeTime = Number(stats.encode_time_ms)
  const dropped = Number(stats.dropped_frame_ratio)
  const safeLogs = redactSensitiveText(logs)
  let issueOwner = 'client'
  let mainIssue = 'Client disconnected or ended the session before Polaris saw a louder host/network signal.'
  let suggestedNextLaunchProfile = 'Retry the same launch profile once, then collect a support bundle if it repeats.'

  if ((Number.isFinite(loss) && loss > 1) || (Number.isFinite(latency) && latency > 45) || /packet loss|network|udp|timeout/i.test(safeLogs)) {
    issueOwner = 'network'
    mainIssue = Number.isFinite(loss) && loss > 1 ? `Network packet loss was ${loss.toFixed(1)}%.` : 'Network latency/transport warnings stood out.'
    suggestedNextLaunchProfile = 'Lower bitrate one step, prefer wired/5 GHz, then retry the same game.'
  }

  if ((Number.isFinite(encodeTime) && encodeTime > 12) || stats.capture_cpu_copy || /encoder|capture|shm|dmabuf|vaapi|nvenc/i.test(safeLogs)) {
    issueOwner = 'host'
    mainIssue = Number.isFinite(encodeTime) && encodeTime > 12 ? `Host encoder time was ${encodeTime.toFixed(1)} ms.` : 'Host capture/encoder path was the loudest signal.'
    suggestedNextLaunchProfile = stats.capture_cpu_copy
      ? 'Try Private Stream (GPU-native) or lower resolution/FPS before changing network settings.'
      : 'Try the low-latency hardware encoder profile or lower resolution/FPS.'
  }

  const qualitySummary = `${Number.isFinite(latency) ? latency.toFixed(1) : 'unknown'} ms latency / ${Number.isFinite(loss) ? loss.toFixed(1) : 'unknown'}% loss / ${Number.isFinite(encodeTime) ? encodeTime.toFixed(1) : 'unknown'} ms encode / ${Number.isFinite(dropped) ? (dropped * 100).toFixed(2) : 'unknown'}% dropped.`
  const report = {
    kind: 'post-session-stream-report',
    status: issueOwner === 'client' ? 'warning' : 'fail',
    issueOwner,
    mainIssue,
    qualitySummary,
    suggestedNextLaunchProfile,
    disconnectReason: redactSensitiveText(disconnectReason),
  }
  report.copyText = [
    'Post-session Stream Report',
    reportLine('Issue owner', issueOwner),
    reportLine('Main issue', mainIssue),
    reportLine('Quality', qualitySummary),
    reportLine('Disconnect reason', report.disconnectReason),
    reportLine('Suggested next launch', suggestedNextLaunchProfile),
    ...(safeLogs.trim() ? [reportLine('Recent evidence', safeLogs.trim().split('\n').slice(-3).join(' | '))] : []),
  ].join('\n')
  return report
}

export function buildSupportSelfTestCopy({ network, controller, postSession } = {}) {
  const sections = []
  if (network) sections.push(['Network Path Tester', network.summary, `Status: ${network.status}`, `Recommended bitrate: ${network.recommendedBitrateKbps || 'unknown'} kbps`, network.nativeEvidenceText].filter(Boolean).join('\n'))
  if (controller) sections.push(['Controller/Input Tester', controller.summary, `Status: ${controller.status}`].join('\n'))
  if (postSession) sections.push(postSession.copyText || ['Post-session Stream Report', postSession.mainIssue].join('\n'))
  return redactSensitiveText(sections.join('\n\n'))
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
