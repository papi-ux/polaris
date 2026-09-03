import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const webSource = (relativePath) => readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web', relativePath),
  'utf8',
)
const templateOf = (source) => source.slice(0, source.indexOf('</template>\n\n<script'))

const surfaces = {
  'views/PinView.vue': 'Devices',
  'views/HomeView.vue': 'System & Updates',
  'views/PasswordView.vue': 'Security',
}

// Devices, System & Updates, and Security follow the same grammar as
// Video/Audio and Doctor & Support: keyed copy, shared inputs and buttons,
// read-only tiles for status, and docs pointers instead of on-page prose.
describe('settings surfaces affordances', () => {
  it.each(Object.keys(surfaces))('%s keeps its template free of hardcoded English sentences', (path) => {
    const offenders = templateOf(webSource(path))
      .split('\n')
      .filter((line) => /^\s*[A-Z][a-z]+(?: [a-z,]+){2,}/.test(line) || />[A-Z][a-z]+(?: [a-z,]+){2,}[^<{]*</.test(line))
    expect(offenders).toEqual([])
  })

  it.each(Object.keys(surfaces))('%s binds dynamic attributes instead of interpolating them', (path) => {
    const offenders = templateOf(webSource(path)).split('\n').filter((line) => /\s[a-zA-Z-]+="[^"]*\{\{/.test(line))
    expect(offenders).toEqual([])
  })

  it('points each surface at its guide', () => {
    expect(webSource('views/PinView.vue')).toContain('href="https://papi-ux.com/docs/devices/"')
    expect(readFileSync(join(process.cwd(), 'docs/devices.md'), 'utf8')).toContain('# Pair and manage devices')
    expect(readFileSync(join(process.cwd(), 'docs/moonlight.md'), 'utf8')).toContain('# Play with Moonlight')
    expect(webSource('views/PinView.vue')).toContain('href="https://papi-ux.com/docs/troubleshooting/#paired-client-gets-permission-denied-403-when-starting-a-stream"')
    expect(webSource('views/HomeView.vue')).toContain('href="https://papi-ux.com/docs/repositories/#after-install-or-upgrade"')
    expect(webSource('views/PasswordView.vue')).toContain('href="https://papi-ux.com/docs/configuration/#rotate-the-web-credentials"')
    expect(webSource('views/PasswordView.vue')).not.toContain('<InfoHint')
    expect(webSource('views/HomeView.vue')).not.toContain('<InfoHint')
    const quickstart = readFileSync(join(process.cwd(), 'docs/quickstart.md'), 'utf8')
    const troubleshooting = readFileSync(join(process.cwd(), 'docs/troubleshooting.md'), 'utf8')
    const repositories = readFileSync(join(process.cwd(), 'docs/repositories.md'), 'utf8')
    const configuration = readFileSync(join(process.cwd(), 'docs/configuration.md'), 'utf8')
    expect(quickstart).toContain('## 4. Pair a client')
    expect(troubleshooting).toContain('## Paired client gets Permission denied (403) when starting a stream')
    expect(repositories).toContain('## After install or upgrade')
    expect(configuration).toContain('## Credential reset')
    expect(configuration).toContain('## Rotate the web credentials')
  })

  it('renders Devices status through StatTile and the pairing routes through SelectableCard', () => {
    const source = webSource('views/PinView.vue')
    expect(source).toContain("import StatTile from '../components/StatTile.vue'")
    expect(source).toContain("import SelectableCard from '../components/SelectableCard.vue'")
    expect(source).toContain('<SelectableCard\n          v-for="method in pairingMethods"')
    expect(source).toContain('<StatTile :label="$t(\'pin.permissions_summary\')"')
    expect(source).toContain('<StatTile v-for="row in hostViewRows"')
    expect(source).not.toContain('rounded-lg border border-storm/15 bg-void/25 px-3 py-2.5')
    expect(source).toContain('role="radio"\n            class="selectable-card focus-ring')
  })

  it('routes every Devices and Security field through the shared input and every action through the shared button', () => {
    const devices = webSource('views/PinView.vue')
    const security = webSource('views/PasswordView.vue')
    expect(devices).not.toContain('w-full rounded-lg border border-storm/50 bg-void/50 px-2.5 py-1.5')
    expect(devices).not.toContain('inline-flex h-9 items-center justify-center rounded-lg bg-ice')
    expect(devices).not.toContain('inline-flex h-9 items-center justify-center rounded-lg border border-storm px-4')
    expect(devices).not.toContain('inline-flex h-9 items-center justify-center rounded-lg bg-danger')
    expect((devices.match(/dashboard-action-button-(primary|ghost|danger)/g) || []).length).toBeGreaterThanOrEqual(9)
    expect((security.match(/class="settings-input pr-20"/g) || []).length).toBe(3)
    expect(security).not.toContain('rounded-lg border border-storm bg-deep px-3 py-2 pr-20')
    expect(security).toContain('class="focus-ring dashboard-action-button dashboard-action-button-primary"')
  })

  it('reads the per-device host view from the settings projection when a device is edited', () => {
    const source = webSource('views/PinView.vue')
    expect(source).toContain("from '../composables/useConfigProjection'")
    expect(source).toContain("from '../client-host-sync.js'")
    expect(source).toContain('hostView.load(client.uuid)')
    expect(source).toContain('data-client-host-view :data-client-host-view-state="hostViewState"')
    expect(source).toContain("$t('pin.host_view_unavailable')")
  })

  it('stops calling a local build the current public release on System & Updates', () => {
    const source = webSource('views/HomeView.vue')
    expect(source).toContain('const buildVersionIsLocal = computed')
    expect(source).toContain("if (buildVersionIsLocal.value) return i18n.t('index.version_local_build')")
    expect(webSource('public/assets/locale/en.json')).toContain('"version_local_build": "Local build, not the packaged release"')
  })
})
