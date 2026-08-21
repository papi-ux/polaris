import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('System page header layout', () => {
  it('keeps the host summary in a responsive three-card grid', () => {
    const home = webSource('views/HomeView.vue')
    const css = webSource('app.css')

    expect(home).toContain('class="page-header system-page-header"')
    expect(home).toContain('class="system-toolbar-notes"')
    expect(home.match(/<article class="header-support-card">/g)).toHaveLength(3)
    expect(css).toContain('.system-toolbar-notes')
    expect(css).toContain('grid-template-columns: repeat(3, minmax(0, 1fr))')
    expect(css).toContain('grid-template-columns: minmax(0, 0.8fr) minmax(36rem, 1.2fr)')
  })

  it('styles each summary item as a bounded card', () => {
    const css = webSource('app.css')

    expect(css).toContain('.header-support-card')
    expect(css).toContain('background: var(--surface-raised)')
    expect(css).toContain('border: 1px solid var(--edge-raised)')
    expect(css).toContain('.header-support-title-row')
    expect(css).toContain('.header-support-value')
    expect(css).toContain('.header-support-copy')
  })
})
