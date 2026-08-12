import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('bounded log-tail view integration', () => {
  it('migrates both log consumers to the versioned bounded helper', () => {
    const troubleshooting = webSource('views/TroubleshootingView.vue')
    const home = webSource('views/HomeView.vue')

    for (const source of [troubleshooting, home]) {
      expect(source).toContain("from '../log-tail-state.js'")
      expect(source).toContain('fetchLogTail(')
      expect(source).not.toContain("fetch('./api/logs',")
      expect(source).not.toContain('fetch("./api/logs",')
    }
  })

  it('exposes truncation and reset state in troubleshooting', () => {
    const troubleshooting = webSource('views/TroubleshootingView.vue')

    expect(troubleshooting).toContain('data-log-tail-truncated')
    expect(troubleshooting).toContain('data-log-tail-reset')
    expect(troubleshooting).toContain('nextState.truncated')
    expect(troubleshooting).toContain('hadCursor && nextState.reset')
  })

  it('reuses the bounded in-memory window for support bundles', () => {
    const troubleshooting = webSource('views/TroubleshootingView.vue')

    expect(troubleshooting).toContain('logs: logs.value')
    expect(troubleshooting).not.toContain("safeFetchText('./api/logs')")
  })
})
