import { describe, expect, it } from 'vitest'
import {
  CUSTOM_EMULATOR,
  blankRomSourceForm,
  customCommandValid,
  parseExtensionList,
  romSourceCountLabel,
  romSourceInstallLabel,
  romSourcePayload,
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
