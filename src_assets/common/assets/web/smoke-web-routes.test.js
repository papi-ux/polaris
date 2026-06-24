import { describe, expect, it } from 'vitest'
import fs from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import {
  collectCriticalHashedAssets,
  getSmokeHelp,
  parseSmokeOptions,
  runLivePreflight,
} from '../../../../scripts/smoke-web-routes.mjs'

describe('smoke web route harness options', () => {
  it('supports explicit live targets and documents self-signed HTTPS handling', () => {
    const options = parseSmokeOptions(['--target-url', 'https://127.0.0.1:47990/', '--no-login'])

    expect(options.mode).toBe('live')
    expect(options.baseURL).toBe('https://127.0.0.1:47990')
    expect(options.noLogin).toBe(true)
    expect(getSmokeHelp()).toContain('--target-url https://127.0.0.1:47990')
    expect(getSmokeHelp()).toContain('self-signed')
  })

  it('supports static build mode for build assets', () => {
    const options = parseSmokeOptions(['--static-dir', 'build/assets/web'])

    expect(options.mode).toBe('static')
    expect(options.staticDir).toBe(path.resolve('build/assets/web'))
    expect(options.baseURL).toBe(null)
  })
})

describe('smoke web route harness asset checks', () => {
  it('discovers hashed module and stylesheet assets from a built index', async () => {
    const root = await fs.mkdtemp(path.join(os.tmpdir(), 'polaris-smoke-assets-'))
    await fs.mkdir(path.join(root, 'assets'), { recursive: true })
    await fs.writeFile(path.join(root, 'index.html'), [
      '<!doctype html>',
      '<script type="module" crossorigin src="./assets/index-AbC123.js"></script>',
      '<link rel="stylesheet" crossorigin href="./assets/index-ZxY987.css">',
    ].join('\n'))
    await fs.writeFile(path.join(root, 'assets/index-AbC123.js'), 'console.log("ok")')
    await fs.writeFile(path.join(root, 'assets/index-ZxY987.css'), 'body{}')

    await expect(collectCriticalHashedAssets(root)).resolves.toEqual([
      '/assets/index-AbC123.js',
      '/assets/index-ZxY987.css',
    ])
  })
})

describe('smoke web route harness live preflight', () => {
  it('reports a clear preflight when the live HTTPS server is missing', async () => {
    await expect(runLivePreflight({
      baseURL: 'https://127.0.0.1:47990',
      request: {
        get: async () => {
          throw Object.assign(new Error('connect ECONNREFUSED 127.0.0.1:47990'), { code: 'ECONNREFUSED' })
        },
      },
    })).rejects.toThrow('Polaris live HTTPS server is not reachable at https://127.0.0.1:47990')
  })
})
