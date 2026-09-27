import { afterEach, describe, expect, it, vi } from 'vitest'

import {
  clearCachedConfig,
  getCachedConfig,
  readConfigOrNull,
  readSettingsRefusal,
  SettingsUnreadableError,
} from './config-cache.js'
import { forgetSettingsRefusal, reportSettingsUnreadable, settingsUnreadable } from './settings-unreadable.js'

// #782: the host answers 503 with this body when it is up but its settings
// store refused polaris.conf. It names the file, the reason and the fix.
const refusal = {
  path: '/srv/polaris/polaris.conf',
  reason: 'It is writable by its group (mode 0664), and the settings store refuses a file another user can change.',
  fix: 'Restrict it with "chmod go-w /srv/polaris/polaris.conf".',
}

function reply(status, body, contentType = 'application/json') {
  return {
    ok: status >= 200 && status < 300,
    status,
    headers: new Headers({ 'content-type': contentType }),
    json: vi.fn(async () => body),
  }
}

describe('readConfigOrNull', () => {
  // Dashboard, System, Apps and Quick Controls read the settings through this and carry on without
  // them when the host does not send them. A refused file still has to reach the banner, and a reply
  // that carries the settings has to clear it.
  afterEach(() => {
    forgetSettingsRefusal()
  })

  it('reads a refused settings file as no settings and reports it to the banner', async () => {
    const response = reply(503, { status: false, error: 'config_unreadable', ...refusal })
    await expect(readConfigOrNull(response)).resolves.toBe(null)
    expect(settingsUnreadable.value).toEqual(refusal)
  })

  it('reads any other failure as no settings and leaves what the banner knows alone', async () => {
    const plain = {
      ok: false,
      status: 500,
      headers: new Headers({ 'content-type': 'text/plain' }),
      json: vi.fn(async () => { throw new SyntaxError('Unexpected token I in JSON') }),
    }
    await expect(readConfigOrNull(plain)).resolves.toBe(null)
    expect(settingsUnreadable.value).toBe(null)

    reportSettingsUnreadable(refusal)
    await expect(readConfigOrNull({ ...plain, json: vi.fn(async () => { throw new SyntaxError('x') }) })).resolves.toBe(null)
    expect(settingsUnreadable.value).toEqual(refusal)
  })

  it('hands back the settings and clears the banner when the file reads', async () => {
    reportSettingsUnreadable(refusal)
    const settings = { status: true, version: '1.4.13' }
    await expect(readConfigOrNull(reply(200, settings))).resolves.toEqual(settings)
    expect(settingsUnreadable.value).toBe(null)
  })
})

describe('config cache', () => {
  afterEach(() => {
    clearCachedConfig()
    delete global.fetch
  })

  it('turns a refused settings file, and only that, into an error that names the file, the reason and the fix', async () => {
    const settings = { status: true, version: '1.4.13' }
    global.fetch = vi.fn()
      .mockResolvedValueOnce(reply(503, { status: false, error: 'Web UI session validation is temporarily unavailable.' }))
      .mockResolvedValueOnce(reply(503, { status: false, error: 'config_unreadable', ...refusal }))
      .mockResolvedValueOnce(reply(200, settings))

    // A 503 without the refusal code is some other failure and stays a plain error.
    const other = await getCachedConfig().catch((error) => error)
    expect(other).toBeInstanceOf(Error)
    expect(other).not.toBeInstanceOf(SettingsUnreadableError)
    expect(other.message).toBe('Config request failed with status 503')

    const failure = await getCachedConfig().catch((error) => error)
    expect(failure).toBeInstanceOf(SettingsUnreadableError)
    expect(failure.refusal).toEqual(refusal)

    // Nothing was cached, so the next read asks the host again, and keeps what it gets.
    await expect(getCachedConfig()).resolves.toEqual(settings)
    await expect(getCachedConfig()).resolves.toEqual(settings)
    expect(global.fetch).toHaveBeenCalledTimes(3)
  })

  it('reads a refusal only from a JSON 503 that carries its code', async () => {
    const body = { status: false, error: 'config_unreadable', ...refusal }
    await expect(readSettingsRefusal(reply(503, body))).resolves.toEqual(refusal)
    await expect(readSettingsRefusal(reply(500, body))).resolves.toBe(null)
    await expect(readSettingsRefusal(reply(503, body, 'text/plain'))).resolves.toBe(null)
    await expect(readSettingsRefusal({ ok: false, status: 503 })).resolves.toBe(null)
    await expect(readSettingsRefusal({
      ok: false,
      status: 503,
      json: async () => { throw new SyntaxError('empty body') },
    })).resolves.toBe(null)
    // A field the host left out reads as empty, never as undefined on the page.
    await expect(readSettingsRefusal(reply(503, { error: 'config_unreadable', reason: 7 }))).resolves.toEqual({
      path: '',
      reason: '',
      fix: '',
    })
  })
})
