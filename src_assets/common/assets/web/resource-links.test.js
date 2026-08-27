import { describe, expect, it } from 'vitest'

import { legalDocs, resources, sponsor } from './resource-links.js'

const all = [...resources, ...legalDocs]

describe('resource links', () => {
  it('offers the docs site for the pages it actually renders', () => {
    // papi-ux.com renders the repository's own docs/, so these are the
    // better destination than the wiki for the same subjects.
    const hrefs = resources.map((entry) => entry.href)
    expect(hrefs).toContain('https://papi-ux.com/docs/')
    expect(hrefs).toContain('https://papi-ux.com/docs/launch-modes/')
    expect(hrefs).toContain('https://papi-ux.com/docs/troubleshooting/')
  })

  it('offers the canonical project entry points', () => {
    const hrefs = resources.map((entry) => entry.href)
    expect(hrefs).toContain('https://papi-ux.com/')
    expect(hrefs).toContain('https://github.com/papi-ux/polaris')
    expect(hrefs).toContain('https://github.com/papi-ux/polaris/releases')
    expect(hrefs).toContain('https://matrix.to/#/#papi-ux:papi-ux.com')
  })

  it('keeps the destinations that only exist on GitHub', () => {
    // Discussions is where the discussion is. The wiki pages live on upstream
    // Apollo's wiki: this repository has no wiki (has_wiki is false), so the
    // former papi-ux/polaris/wiki targets were dead links. This pins the
    // working upstream destinations so a sweep does not "repatriate" them
    // into 404s.
    const hrefs = resources.map((entry) => entry.href)
    expect(hrefs).toContain('https://github.com/papi-ux/polaris/discussions')
    expect(hrefs).toContain('https://github.com/ClassicOldSong/Apollo/wiki')
    expect(hrefs).toContain('https://github.com/ClassicOldSong/Apollo/wiki/Stuttering-Clinic')
  })

  it('gives every link a label key and an absolute https target', () => {
    for (const entry of all) {
      expect(entry.labelKey, JSON.stringify(entry)).toMatch(/^resource_card\./)
      expect(entry.href, JSON.stringify(entry)).toMatch(/^https:\/\//)
    }
  })

  it('lists each destination once', () => {
    const hrefs = all.map((entry) => entry.href)
    expect(new Set(hrefs).size).toBe(hrefs.length)
  })

  it('keeps sponsorship separate from primary resources', () => {
    expect(sponsor).toEqual({
      href: 'https://github.com/sponsors/papi-ux',
      labelKey: 'resource_card.sponsor',
      ariaLabelKey: 'resource_card.sponsor_desc'
    })
    expect(all.map((entry) => entry.href)).not.toContain(sponsor.href)
  })
})
