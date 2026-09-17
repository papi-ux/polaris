export const CUSTOM_EMULATOR = 'custom'
export const ROM_PLACEHOLDER = '{rom}'

/** "nsp, xci .NCA" and friends into a clean, unique, lower-case list. */
export function parseExtensionList(text = '') {
  const seen = new Set()
  return String(text || '')
    .split(/[\s,;]+/)
    .map((part) => part.toLowerCase().replace(/^\.+/, '').replace(/[^a-z0-9]/g, ''))
    .filter((part) => {
      if (!part || seen.has(part)) return false
      seen.add(part)
      return true
    })
}

/** A custom command names the emulator and carries the placeholder exactly once. */
export function customCommandValid(command = '') {
  const trimmed = String(command || '').trim()
  const first = trimmed.indexOf(ROM_PLACEHOLDER)
  if (first < 0) return false
  if (trimmed.indexOf(ROM_PLACEHOLDER, first + ROM_PLACEHOLDER.length) >= 0) return false
  return trimmed !== ROM_PLACEHOLDER
}

export function blankRomSourceForm(presets = []) {
  return {
    path: '',
    emulator: presets[0]?.id || 'eden',
    launcher: '',
    command: '',
    extensions: '',
  }
}

/** The first thing wrong with the form, in the words the console shows, or an empty string. */
export function validateRomSourceForm(form = {}, presets = []) {
  const path = String(form.path || '').trim()
  if (!path) return 'Give the folder that holds the games.'
  if (!path.startsWith('/') && !path.startsWith('~')) return 'Give the folder as an absolute path, or start it with ~/'
  const emulator = String(form.emulator || '').trim()
  if (emulator === CUSTOM_EMULATOR) {
    if (!customCommandValid(form.command)) return 'A custom command names the emulator and carries {rom} exactly once.'
    if (!parseExtensionList(form.extensions).length) return 'List the file extensions to look for, for example: sfc, smc, zip'
    return ''
  }
  if (!presets.some((preset) => preset.id === emulator)) return 'Pick the emulator that loads these files.'
  return ''
}

/** What the host is asked to store: presets never carry a command, custom folders never a launcher. */
export function romSourcePayload(form = {}) {
  const emulator = String(form.emulator || '').trim()
  const payload = { path: String(form.path || '').trim(), emulator }
  if (emulator === CUSTOM_EMULATOR) {
    payload.command = String(form.command || '').trim()
    payload.extensions = parseExtensionList(form.extensions)
    return payload
  }
  const launcher = String(form.launcher || '').trim()
  if (launcher) payload.launcher = launcher
  const extensions = parseExtensionList(form.extensions)
  if (extensions.length) payload.extensions = extensions
  return payload
}

/** "Eden via Flatpak", "Eden at ~/Apps/Eden.AppImage", "Eden not found". */
export function romSourceInstallLabel(source = {}) {
  if (source.emulator === CUSTOM_EMULATOR) return 'Custom command'
  const label = source.label || source.emulator || 'Emulator'
  switch (source.install?.kind) {
    case 'native':
      return `${label} on PATH`
    case 'flatpak':
      return `${label} via Flatpak`
    case 'launcher':
      return `${label} at ${source.install.location}`
    default:
      return `${label} not found`
  }
}

/** Everything that stops a game in this folder from booting: the folder's own warning first. */
export function romSourceProblems(source = {}) {
  const problems = []
  if (source.warning) problems.push(source.warning)
  for (const check of source.prerequisites || []) {
    if (check?.severity === 'warning' && check.message) problems.push(check.message)
  }
  return problems
}

export function romSourceReady(source = {}) {
  return romSourceProblems(source).length === 0
}

export function romSourceStatus(source = {}) {
  const problems = romSourceProblems(source)
  return problems.length ? problems[0] : 'Ready'
}

export function romSourceCountLabel(source = {}) {
  if (typeof source.rom_count !== 'number') return 'Scan to count'
  return `${source.rom_count} game${source.rom_count === 1 ? '' : 's'}`
}

/**
 * What a folder card or the chosen preset can offer for installing its emulator:
 * 'installing' while the host installs it, 'offer' when it is missing and Flathub has
 * it, or '' when there is nothing to install. A folder that names its own emulator file
 * keeps that file, so a Flatpak would not change what runs and none is offered.
 */
export function romEmulatorInstallState(entry = {}) {
  const emulator = entry.emulator ?? entry.id
  if (!emulator || emulator === CUSTOM_EMULATOR) return ''
  if (entry.install_job?.state === 'installing') return 'installing'
  if (entry.install?.kind !== 'missing' || !entry.installable) return ''
  if (String(entry.launcher || '').trim()) return ''
  return 'offer'
}

/** The emulator a card installs: a folder names it, a preset is it. */
export function romEmulatorId(entry = {}) {
  return entry.emulator ?? entry.id ?? ''
}

/** Why the last install failed, while the emulator is still missing; Flatpak's own reason when it gave one. */
export function romEmulatorInstallFailure(entry = {}) {
  if (entry.install_job?.state !== 'failed' || entry.install?.kind !== 'missing') return ''
  return entry.install_job.message || 'The install from Flathub failed.'
}
