import { effectScope } from 'vue'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { forgetInstallJobs, romInstallsPending, useRomSources } from './composables/useRomSources'
import {
  CUSTOM_EMULATOR,
  blankRomSourceForm,
  customCommandValid,
  parseExtensionList,
  romEmulatorId,
  romEmulatorInstallFailure,
  romEmulatorInstallState,
  romSourceCountLabel,
  romSourceInstallLabel,
  romSourcePayload,
  romSourceProblems,
  romSourceReady,
  romSourceStatus,
  validateRomSourceForm,
} from './rom-sources'

const presets = [
  { id: 'eden', label: 'Eden', platform: 'Nintendo Switch' },
  { id: 'dolphin', label: 'Dolphin', platform: 'GameCube and Wii' },
]

describe('ROM folder forms', () => {
  it('cleans an extension list the way the host does', () => {
    expect(parseExtensionList('nsp, xci .NCA;nro  nsp')).toEqual(['nsp', 'xci', 'nca', 'nro'])
    expect(parseExtensionList(' , ')).toEqual([])
    expect(parseExtensionList(undefined)).toEqual([])
  })

  it('requires the placeholder exactly once in a custom command', () => {
    expect(customCommandValid('retroarch -f -L /cores/snes9x_libretro.so {rom}')).toBe(true)
    expect(customCommandValid('  my-emu {rom} --fullscreen ')).toBe(true)
    expect(customCommandValid('')).toBe(false)
    expect(customCommandValid('{rom}')).toBe(false)
    expect(customCommandValid('retroarch -f')).toBe(false)
    expect(customCommandValid('emu {rom} {rom}')).toBe(false)
  })

  it('names the first problem with the form', () => {
    expect(validateRomSourceForm({ path: '', emulator: 'eden' }, presets)).toMatch(/folder/i)
    expect(validateRomSourceForm({ path: 'roms/switch', emulator: 'eden' }, presets)).toMatch(/absolute path/)
    expect(validateRomSourceForm({ path: '~/roms', emulator: 'ryujinx' }, presets)).toMatch(/Pick the emulator/)
    expect(validateRomSourceForm({ path: '/roms', emulator: 'eden' }, presets)).toBe('')
    expect(validateRomSourceForm({ path: '/roms', emulator: CUSTOM_EMULATOR, command: 'retroarch -f' }, presets)).toMatch(/exactly once/)
    expect(validateRomSourceForm({ path: '/roms', emulator: CUSTOM_EMULATOR, command: 'retroarch -f {rom}', extensions: '' }, presets)).toMatch(/extensions/)
    expect(validateRomSourceForm({ path: '/roms', emulator: CUSTOM_EMULATOR, command: 'retroarch -f {rom}', extensions: 'sfc' }, presets)).toBe('')
  })

  it('sends the host only what applies to the kind of folder', () => {
    expect(romSourcePayload({ path: ' ~/roms/switch ', emulator: 'eden', launcher: '', command: 'ignored {rom}', extensions: '' }))
      .toEqual({ path: '~/roms/switch', emulator: 'eden' })
    expect(romSourcePayload({ path: '/roms/wiiu', emulator: 'cemu', launcher: ' ~/Apps/Cemu.AppImage ', extensions: 'wua' }))
      .toEqual({ path: '/roms/wiiu', emulator: 'cemu', launcher: '~/Apps/Cemu.AppImage', extensions: ['wua'] })
    expect(romSourcePayload({ path: '/roms/snes', emulator: CUSTOM_EMULATOR, launcher: '/ignored', command: ' retroarch -f {rom} ', extensions: 'sfc, smc' }))
      .toEqual({ path: '/roms/snes', emulator: CUSTOM_EMULATOR, command: 'retroarch -f {rom}', extensions: ['sfc', 'smc'] })
  })

  it('names what stops a folder from booting games, warnings before hints', () => {
    const ready = { label: 'Eden', prerequisites: [] }
    const missingKeys = { label: 'Eden', prerequisites: [{ id: 'eden_keys_missing', severity: 'warning', message: 'Eden has no prod.keys, so no game will boot.', action: 'Copy them' }] }
    const hintOnly = { label: 'Cemu', prerequisites: [{ id: 'cemu_keys_missing', severity: 'info', message: 'Cemu has no keys.txt', action: 'Put keys.txt' }] }
    const gone = { label: 'Eden', warning: 'Folder not found: /roms', prerequisites: [{ id: 'eden_keys_missing', severity: 'warning', message: 'no keys', action: '' }] }
    expect(romSourceReady(ready)).toBe(true)
    expect(romSourceStatus(ready)).toBe('Ready')
    expect(romSourceReady(missingKeys)).toBe(false)
    expect(romSourceStatus(missingKeys)).toBe('Eden has no prod.keys, so no game will boot.')
    expect(romSourceReady(hintOnly)).toBe(true)
    expect(romSourceProblems(gone)).toEqual(['Folder not found: /roms', 'no keys'])
    expect(romSourceStatus({})).toBe('Ready')
  })

  it('starts a blank form on the first preset', () => {
    expect(blankRomSourceForm(presets).emulator).toBe('eden')
    expect(blankRomSourceForm([]).emulator).toBe('eden')
    expect(blankRomSourceForm(presets).path).toBe('')
  })

  it('describes where the emulator was found', () => {
    expect(romSourceInstallLabel({ emulator: 'eden', label: 'Eden', install: { kind: 'native', location: '/usr/bin/eden' } })).toBe('Eden on PATH')
    expect(romSourceInstallLabel({ emulator: 'eden', label: 'Eden', install: { kind: 'flatpak', location: 'dev.eden_emu.eden' } })).toBe('Eden via Flatpak')
    expect(romSourceInstallLabel({ emulator: 'eden', label: 'Eden', install: { kind: 'launcher', location: '/opt/Eden.AppImage' } })).toBe('Eden at /opt/Eden.AppImage')
    expect(romSourceInstallLabel({ emulator: 'eden', label: 'Eden', install: { kind: 'missing', location: '' } })).toBe('Eden not found')
    expect(romSourceInstallLabel({ emulator: CUSTOM_EMULATOR, label: 'Custom command' })).toBe('Custom command')
    expect(romSourceCountLabel({})).toBe('Scan to count')
    expect(romSourceCountLabel({ rom_count: 1 })).toBe('1 game')
    expect(romSourceCountLabel({ rom_count: 12 })).toBe('12 games')
  })
})

describe('the folder list while an emulator installs', () => {
  afterEach(() => {
    forgetInstallJobs()
    vi.useRealTimers()
    vi.restoreAllMocks()
    delete global.fetch
  })

  it('stops reading the folder list once the page is gone', async () => {
    vi.useFakeTimers()
    const installing = {
      status: true,
      presets: [{ id: 'eden', label: 'Eden', install: { kind: 'missing' }, installable: true, install_job: { state: 'installing' } }],
      folders: [],
    }
    global.fetch = vi.fn(() => Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(installing) }))
    const scope = effectScope()
    let rom
    scope.run(() => { rom = useRomSources({ pollIntervalMs: 2000 }) })

    await rom.load()
    expect(global.fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(2100)
    expect(global.fetch).toHaveBeenCalledTimes(2)

    // A load already out when the page goes away must not start the timer again.
    const pending = rom.load()
    scope.stop()
    await pending
    await vi.advanceTimersByTimeAsync(10000)
    expect(global.fetch).toHaveBeenCalledTimes(3)
  })

  it('reports an install that finished while the page was closed, once', async () => {
    const preset = { id: 'eden', label: 'Eden', installable: true }
    let job = { state: 'installing', message: 'Installing Eden from Flathub.', started_at: 1, finished_at: 0 }
    global.fetch = vi.fn(() => Promise.resolve({
      ok: true,
      status: 200,
      json: () => Promise.resolve({ status: true, presets: [{ ...preset, install: { kind: job.state === 'installed' ? 'flatpak' : 'missing' }, install_job: job }], sources: [] }),
    }))

    // The player starts the install and leaves the Apps page before it is done.
    const away = effectScope()
    let first
    away.run(() => { first = useRomSources({ pollIntervalMs: 60000 }) })
    await first.load()
    away.stop()

    job = { state: 'installed', message: 'Eden is installed.', started_at: 1, finished_at: 2 }

    // Coming back builds the page again, and the finish is still reported.
    const back = effectScope()
    let second
    const finished = []
    back.run(() => { second = useRomSources({ pollIntervalMs: 60000 }) })
    second.onInstallFinished((entry) => finished.push(entry))
    await second.load()
    await second.load()
    back.stop()

    expect(finished).toEqual([{ emulator: 'eden', job }])
    expect(romInstallsPending()).toBe(false)
  })

  it('keeps a finish that arrived after the page closed for the next visit', async () => {
    const preset = { id: 'eden', label: 'Eden', installable: true }
    let job = { state: 'installing', message: 'Installing Eden from Flathub.', started_at: 1, finished_at: 0 }
    let release
    const answer = () => ({ ok: true, status: 200, json: () => Promise.resolve({ status: true, presets: [{ ...preset, install: { kind: 'missing' }, install_job: job }], sources: [] }) })
    global.fetch = vi.fn(() => Promise.resolve(answer()))

    const away = effectScope()
    let first
    away.run(() => { first = useRomSources({ pollIntervalMs: 60000 }) })
    await first.load()
    expect(romInstallsPending()).toBe(true)

    // A poll is still out when the player leaves, and its answer says the install finished.
    job = { state: 'installed', message: 'Eden is installed.', started_at: 1, finished_at: 2 }
    global.fetch = vi.fn(() => new Promise((resolve) => { release = () => resolve(answer()) }))
    const late = first.load()
    away.stop()
    release()
    await late
    expect(romInstallsPending()).toBe(true)

    global.fetch = vi.fn(() => Promise.resolve(answer()))
    const back = effectScope()
    let second
    const finished = []
    back.run(() => { second = useRomSources({ pollIntervalMs: 60000 }) })
    second.onInstallFinished((entry) => finished.push(entry))
    await second.load()
    back.stop()
    expect(finished).toEqual([{ emulator: 'eden', job }])
  })
})

describe('installing a missing emulator from Flathub', () => {
  const missing = { kind: 'missing', location: '' }

  it('offers the install only for a missing preset emulator Flathub can provide', () => {
    expect(romEmulatorInstallState({ emulator: 'eden', install: missing, installable: true })).toBe('offer')
    expect(romEmulatorInstallState({ id: 'eden', install: missing, installable: true })).toBe('offer')
    expect(romEmulatorInstallState({ emulator: 'eden', install: missing, installable: false })).toBe('')
    expect(romEmulatorInstallState({ emulator: 'eden', install: { kind: 'flatpak', location: 'dev.eden_emu.eden' }, installable: true })).toBe('')
    expect(romEmulatorInstallState({ emulator: CUSTOM_EMULATOR, install: missing, installable: true })).toBe('')
    expect(romEmulatorInstallState({})).toBe('')
    // A folder that names its own emulator file keeps running that file.
    expect(romEmulatorInstallState({ emulator: 'eden', launcher: '/opt/Apps/Eden.AppImage', install: { kind: 'missing', location: '/opt/Apps/Eden.AppImage' }, installable: true })).toBe('')
  })

  it('shows a running install whatever the emulator looks like meanwhile', () => {
    const job = { state: 'installing', message: 'Installing Eden from Flathub.' }
    expect(romEmulatorInstallState({ emulator: 'eden', install: missing, installable: true, install_job: job })).toBe('installing')
    expect(romEmulatorInstallState({ emulator: 'eden', install: missing, installable: false, install_job: job })).toBe('installing')
  })

  it('keeps a failure only while the emulator is still missing', () => {
    const failed = { state: 'failed', message: 'Installing DuckStation from Flathub failed: Nothing matches org.duckstation.DuckStation in remote flathub' }
    expect(romEmulatorInstallFailure({ emulator: 'duckstation', install: missing, install_job: failed })).toBe(failed.message)
    expect(romEmulatorInstallFailure({ emulator: 'duckstation', install: missing, install_job: { state: 'failed' } })).toBe('The install from Flathub failed.')
    expect(romEmulatorInstallFailure({ emulator: 'duckstation', install: missing, install_job: { state: 'failed' } }, 'Flathub-Installation fehlgeschlagen.')).toBe('Flathub-Installation fehlgeschlagen.')
    expect(romEmulatorInstallFailure({ emulator: 'duckstation', install: { kind: 'native', location: '/usr/bin/duckstation-qt' }, install_job: failed })).toBe('')
    expect(romEmulatorInstallFailure({ emulator: 'eden', install: missing, install_job: { state: 'installed', message: 'Eden is installed.' } })).toBe('')
  })

  it('names the emulator a folder or a preset installs', () => {
    expect(romEmulatorId({ id: 'folder-1', emulator: 'eden' })).toBe('eden')
    expect(romEmulatorId({ id: 'dolphin' })).toBe('dolphin')
    expect(romEmulatorId({})).toBe('')
  })
})
