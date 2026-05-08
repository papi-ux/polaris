import { readFileSync } from 'node:fs'
import { join } from 'node:path'

describe('app chrome version display', () => {
  it('does not show a stale hardcoded version while waiting for host config', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/App.vue'), 'utf8')

    expect(source).not.toContain("const appVersion = ref('1.0.0')")
    expect(source).toContain("const appVersion = ref('')")
    expect(source).toContain("appVersion.value ? `v${appVersion.value}` : 'v...'")
  })

  it('refreshes the version after authenticated navigation exposes the app shell', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/App.vue'), 'utf8')

    expect(source).toContain("import { getCachedConfig } from './config-cache.js'")
    expect(source).toContain('async function loadAppVersion()')
    expect(source).toContain('watch(showNav, (visible) => {')
    expect(source).toContain('void loadAppVersion()')
  })
})
