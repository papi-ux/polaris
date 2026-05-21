import { readFileSync } from 'node:fs'
import { join } from 'node:path'

describe('Library import console placement', () => {
  it('renders the Import Games console before the library body so it opens at the top', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/AppsView.vue'), 'utf8')

    const importConsoleIndex = source.indexOf('class="section-card library-import-console"')
    const libraryBodyIndex = source.indexOf('class="grid gap-4 xl:grid-cols-[minmax(0,1.2fr)_minmax(300px,0.8fr)]"')

    expect(importConsoleIndex).toBeGreaterThan(-1)
    expect(libraryBodyIndex).toBeGreaterThan(-1)
    expect(importConsoleIndex).toBeLessThan(libraryBodyIndex)
  })
})
