import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it, vi } from 'vitest'

import { requestHostRestart } from './restart-host.js'

describe('host restart flow', () => {
  it('accepts the restart response and polls readiness instead of forcing a reload during transient disconnects', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce({ ok: true, status: 202 })
      .mockRejectedValueOnce(new TypeError('listener restarting'))
      .mockResolvedValueOnce({ ok: false, status: 503 })
      .mockResolvedValueOnce({ ok: true, status: 200 })
    const sleep = vi.fn(() => Promise.resolve())
    const reload = vi.fn()

    const result = await requestHostRestart({
      fetchImpl,
      sleep,
      reload,
      readyPollAttempts: 3,
      readyPollInitialDelayMs: 25,
      readyPollIntervalMs: 50,
    })

    expect(result).toEqual({ accepted: true, ready: true, attempts: 3 })
    expect(reload).not.toHaveBeenCalled()
    expect(fetchImpl).toHaveBeenNthCalledWith(1, './api/restart', {
      credentials: 'include',
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
    })
    expect(fetchImpl).toHaveBeenNthCalledWith(2, './api/config', {
      credentials: 'include',
      cache: 'no-store',
    })
    expect(fetchImpl).toHaveBeenNthCalledWith(4, './api/config', {
      credentials: 'include',
      cache: 'no-store',
    })
    expect(sleep).toHaveBeenCalledWith(25)
    expect(sleep).toHaveBeenCalledWith(50)
  })

  it('times out quietly without reloading into a half-dead HTTPS listener', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce({ ok: true, status: 200 })
      .mockRejectedValue(new TypeError('connection refused'))
    const sleep = vi.fn(() => Promise.resolve())
    const reload = vi.fn()
    const onTimeout = vi.fn()

    const result = await requestHostRestart({
      fetchImpl,
      sleep,
      reload,
      onTimeout,
      readyPollAttempts: 2,
      readyPollInitialDelayMs: 10,
      readyPollIntervalMs: 20,
    })

    expect(result).toEqual({ accepted: true, ready: false, attempts: 2 })
    expect(onTimeout).toHaveBeenCalledTimes(1)
    expect(reload).not.toHaveBeenCalled()
  })

  // #782: a bare 503 from a refused settings file kept the restart spinning
  // until it timed out. The host is up; only its settings file is unreadable.
  it('reports a host that answers with a refused settings file as up and passes the refusal on', async () => {
    const refusal = {
      path: '/srv/polaris/polaris.conf',
      reason: 'It is writable by its group (mode 0664), and the settings store refuses a file another user can change.',
      fix: 'Restrict it with "chmod go-w /srv/polaris/polaris.conf".',
    }
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce({ ok: true, status: 202 })
      // Any other 503 is still a host that is not ready yet.
      .mockResolvedValueOnce({
        ok: false,
        status: 503,
        headers: new Headers({ 'content-type': 'application/json' }),
        json: async () => ({ status: false, error: 'Web UI session validation is temporarily unavailable.' }),
      })
      .mockResolvedValueOnce({
        ok: false,
        status: 503,
        headers: new Headers({ 'content-type': 'application/json' }),
        json: async () => ({ status: false, error: 'config_unreadable', ...refusal }),
      })
    const onReady = vi.fn()
    const onTimeout = vi.fn()

    const result = await requestHostRestart({
      fetchImpl,
      sleep: vi.fn(() => Promise.resolve()),
      onReady,
      onTimeout,
      readyPollAttempts: 3,
    })

    expect(result).toEqual({ accepted: true, ready: true, attempts: 2, settingsUnreadable: refusal })
    expect(onReady).toHaveBeenCalledWith({ ready: true, attempts: 2, settingsUnreadable: refusal })
    expect(onTimeout).not.toHaveBeenCalled()
    expect(fetchImpl).toHaveBeenCalledTimes(3)
  })

  it('wires config and troubleshooting restart actions through the shared readiness flow', () => {
    for (const view of ['ConfigView.vue', 'TroubleshootingView.vue']) {
      const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views', view), 'utf8')
      expect(source).toContain("import { requestHostRestart } from '../restart-host.js'")
      expect(source).toContain('requestHostRestart(')
      expect(source).not.toContain('location.reload()')
    }
  })

  it('backend restart endpoint responds before scheduling process termination', () => {
    const source = readFileSync(join(process.cwd(), 'src/confighttp.cpp'), 'utf8')
    const start = source.indexOf('void restart(resp_https_t response, req_https_t request)')
    const end = source.indexOf('/**', start + 1)
    const restartHandler = source.slice(start, end)

    expect(restartHandler).toContain('output_tree["status"] = true')
    expect(restartHandler).toContain('output_tree["restarting"] = true')
    expect(restartHandler.indexOf('send_response(response, output_tree)')).toBeGreaterThan(-1)
    expect(restartHandler.indexOf('send_response(response, output_tree)')).toBeLessThan(restartHandler.indexOf('std::thread'))
    expect(restartHandler.indexOf('send_response(response, output_tree)')).toBeLessThan(restartHandler.indexOf('platf::restart()'))
  })
})
