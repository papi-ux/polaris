import { readFileSync } from 'node:fs'
import { join } from 'node:path'

import { describe, expect, it } from 'vitest'

const webSource = (relativePath) => readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web', relativePath),
  'utf8'
)

describe('System resource sponsorship', () => {
  it('renders the sponsor as a quiet, accessible footer link', () => {
    const home = webSource('views/HomeView.vue')
    const footerStart = home.indexOf('<footer class="system-resource-footer">')
    const footerEnd = home.indexOf('</footer>', footerStart)
    const resourcesFooter = home.slice(footerStart, footerEnd)

    expect(footerStart).toBeGreaterThan(-1)
    expect(resourcesFooter).toContain(':href="sponsor.href"')
    expect(resourcesFooter).toContain(':aria-label="$t(sponsor.ariaLabelKey)"')
    expect(resourcesFooter).toContain('rel="noopener noreferrer"')
    expect(resourcesFooter).toContain('aria-hidden="true"')
    expect(resourcesFooter).toContain('{{ $t(sponsor.labelKey) }}')
    expect(resourcesFooter).toContain('text-xs')
  })

  it('does not repeat the sponsor prompt on onboarding and recovery cards', () => {
    expect(webSource('ResourceCard.vue')).not.toContain('sponsor')
  })
})
