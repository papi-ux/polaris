import { readFileSync } from 'node:fs'
import { join } from 'node:path'

describe('Library import console placement', () => {
  it('renders the Import Games console before the library body so it opens at the top', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/AppsView.vue'), 'utf8')

    const importConsoleIndex = source.indexOf('class="section-card library-import-console"')
    const libraryBodyIndex = source.indexOf('data-library-body')

    expect(importConsoleIndex).toBeGreaterThan(-1)
    expect(libraryBodyIndex).toBeGreaterThan(-1)
    expect(importConsoleIndex).toBeLessThan(libraryBodyIndex)
  })
})
