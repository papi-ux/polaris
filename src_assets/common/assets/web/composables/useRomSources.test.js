import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { effectScope } from 'vue'

import { forgetInstallJobs, useRomSources } from './useRomSources'

function respond(status, body) {
  return Promise.resolve({ ok: status >= 200 && status < 300, status, json: () => Promise.resolve(body) })
}

function listing(jobState, message = '') {
  const job = jobState ? { state: jobState, message, started_at: 1, finished_at: jobState === 'installing' ? 0 : 2 } : null
  return {
    status: true,
    presets: [
      { id: 'eden', label: 'Eden', install: { kind: jobState === 'installed' ? 'flatpak' : 'missing', location: '' }, installable: true, install_job: job },
      { id: 'dolphin', label: 'Dolphin', install: { kind: 'missing', location: '' }, installable: true, install_job: null },
    ],
    sources: [
      { id: 'folder-1', emulator: 'eden', label: 'Eden', install: { kind: 'missing', location: '' }, installable: true, install_job: job },
    ],
  }
}

async function settle() {
  for (let i = 0; i < 6; i++) await Promise.resolve()
}

describe('useRomSources emulator installs', () => {
  let scope

  beforeEach(() => {
    vi.useFakeTimers()
    scope = effectScope()
  })

  afterEach(() => {
    scope.stop()
    forgetInstallJobs()
    vi.useRealTimers()
    delete global.fetch
  })

  it('asks the host to install, follows the job and reports the finish once', async () => {
    const answers = [listing('installing', 'Installing Eden from Flathub.'), listing('installing', 'Installing Eden from Flathub.'), listing('installed', 'Eden is installed.')]
    global.fetch = vi.fn((url) => {
      if (url === './api/library/emulators/install') {
        return respond(202, { status: true, install_job: { state: 'installing', message: 'Installing Eden from Flathub.' } })
      }
      return respond(200, answers.length > 1 ? answers.shift() : answers[0])
    })
    const rom = scope.run(() => useRomSources({ pollIntervalMs: 1000 }))
    const finished = vi.fn()
    rom.onInstallFinished(finished)

    const request = rom.install('eden')
    expect(rom.installRequests.value).toEqual({ eden: true })
    expect(await request).toBe(true)
    expect(rom.installRequests.value).toEqual({})
    const [url, options] = global.fetch.mock.calls[0]
    expect(url).toBe('./api/library/emulators/install')
    expect(options.method).toBe('POST')
    expect(options.headers['Content-Type']).toBe('application/json')
    expect(JSON.parse(options.body)).toEqual({ emulator: 'eden' })
    expect(rom.presets.value[0].install_job.state).toBe('installing')
    expect(rom.sources.value[0].install_job.state).toBe('installing')
    expect(finished).not.toHaveBeenCalled()

    await vi.advanceTimersByTimeAsync(1000)
    await settle()
    expect(finished).not.toHaveBeenCalled()
    await vi.advanceTimersByTimeAsync(1000)
    await settle()
    expect(rom.presets.value[0].install.kind).toBe('flatpak')
    expect(finished).toHaveBeenCalledTimes(1)
    expect(finished).toHaveBeenCalledWith({ emulator: 'eden', job: expect.objectContaining({ state: 'installed', message: 'Eden is installed.' }) })

    // Nothing is running any more, so the list is not read again.
    const reads = global.fetch.mock.calls.length
    await vi.advanceTimersByTimeAsync(5000)
    await settle()
    expect(global.fetch.mock.calls.length).toBe(reads)
    expect(finished).toHaveBeenCalledTimes(1)
  })

  it('reports an install that was over before the list was read, and one already running at page load', async () => {
    let answer = listing('failed', 'Installing Eden from Flathub failed: Nothing matches dev.eden_emu.eden in remote flathub')
    global.fetch = vi.fn((url) => (url === './api/library/emulators/install' ? respond(202, { status: true }) : respond(200, answer)))
    const rom = scope.run(() => useRomSources({ pollIntervalMs: 1000 }))
    const finished = vi.fn()
    rom.onInstallFinished(finished)

    await rom.install('eden')
    expect(finished).toHaveBeenCalledWith({ emulator: 'eden', job: expect.objectContaining({ state: 'failed' }) })

    finished.mockClear()
    answer = listing('installing', 'Installing Eden from Flathub.')
    await rom.load()
    answer = listing('installed', 'Eden is installed.')
    await vi.advanceTimersByTimeAsync(1000)
    await settle()
    expect(finished).toHaveBeenCalledTimes(1)
  })

  it('shows why the host would not start an install', async () => {
    global.fetch = vi.fn((url) => (url === './api/library/emulators/install'
      ? respond(409, { status: false, error: 'Eden is already being installed.' })
      : respond(200, listing('installing', 'Installing Eden from Flathub.'))))
    const rom = scope.run(() => useRomSources({ pollIntervalMs: 1000 }))

    expect(await rom.install('eden')).toBe(false)
    expect(rom.error.value).toBe('Eden is already being installed.')
    expect(rom.installRequests.value).toEqual({})
  })

  it('stops following installs when the view goes away', async () => {
    global.fetch = vi.fn(() => respond(200, listing('installing', 'Installing Eden from Flathub.')))
    const rom = scope.run(() => useRomSources({ pollIntervalMs: 1000 }))
    await rom.load()
    expect(global.fetch).toHaveBeenCalledTimes(1)
    scope.stop()
    await vi.advanceTimersByTimeAsync(5000)
    await settle()
    expect(global.fetch).toHaveBeenCalledTimes(1)
  })
})
