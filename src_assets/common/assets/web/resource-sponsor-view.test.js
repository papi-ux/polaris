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
    const resourcesSection = home.slice(
      home.indexOf('<div class="section-kicker">Resources</div>'),
      home.indexOf('<div class="section-kicker">Legal</div>')
    )

    expect(resourcesSection).toContain(':href="sponsor.href"')
    expect(resourcesSection).toContain(':aria-label="$t(sponsor.ariaLabelKey)"')
    expect(resourcesSection).toContain('rel="noopener noreferrer"')
    expect(resourcesSection).toContain('aria-hidden="true"')
    expect(resourcesSection).toContain('{{ $t(sponsor.labelKey) }}')
    expect(resourcesSection).toContain('text-xs')
  })

  it('does not repeat the sponsor prompt on onboarding and recovery cards', () => {
    expect(webSource('ResourceCard.vue')).not.toContain('sponsor')
  })
})
