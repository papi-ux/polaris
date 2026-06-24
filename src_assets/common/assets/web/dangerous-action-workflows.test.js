import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('dangerous host action workflows', () => {
  it('uses the reusable confirmation dialog instead of native confirms for host-affecting views', () => {
    for (const relativePath of ['views/DashboardView.vue', 'views/TroubleshootingView.vue', 'components/QuickControls.vue']) {
      const source = webSource(relativePath)
      expect(source).toContain("ConfirmActionDialog")
      expect(source).not.toMatch(/window\.confirm\(|(?<!\.)\bconfirm\(/)
    }
  })

  it('gives each confirmed workflow impact copy plus async feedback hooks', () => {
    const dashboard = webSource('views/DashboardView.vue')
    expect(dashboard).toContain('disconnectConfirmOpen')
    expect(dashboard).toContain('disconnectingClient')
    expect(dashboard).toContain('dashboard.disconnect_client_impact_stream')
    expect(dashboard).toContain('dashboard.disconnect_client_success')
    expect(dashboard).toContain('dashboard.recording_start_success')

    const troubleshooting = webSource('views/TroubleshootingView.vue')
    expect(troubleshooting).toContain('confirmActionOpen')
    expect(troubleshooting).toContain('pendingConfirmedAction')
    expect(troubleshooting).toContain('troubleshooting.restart_polaris_impact_streams')
    expect(troubleshooting).toContain('troubleshooting.force_close_success')
    expect(troubleshooting).toContain('troubleshooting.quit_polaris_error')

    const quickControls = webSource('components/QuickControls.vue')
    expect(quickControls).toContain('pendingRestartToggle')
    expect(quickControls).toContain('quick_controls.restart_confirm_impact')
    expect(quickControls).toContain('quick_controls.save_failed')
  })
})
