import { afterEach, describe, expect, it } from 'vitest'
import {
  EMBED_STORAGE_KEY, artLaunchHref, embeddedClient, embeddedTheme, forgetEmbeddedTheme, initClientEmbed,
} from './client-embed.js'

const themes = ['polaris', 'portable-chrome', 'oled', 'miami', 'director', 'high-contrast']
function memoryStorage() {
  const items = new Map()
  return { getItem: key => (items.has(key) ? items.get(key) : null), setItem: (key, value) => items.set(key, String(value)), items }
}

afterEach(() => { initClientEmbed({ search: '', storage: memoryStorage(), themes }) })

describe('a console opened inside a client app', () => {
  it('knows it is in Nova, and Nova\'s theme, from the first load and keeps both for the visit', () => {
    const storage = memoryStorage()
    expect(initClientEmbed({ search: '?client=nova&theme=director', storage, themes })).toEqual({ client: 'nova', theme: 'director' })
    expect([embeddedClient(), embeddedTheme()]).toEqual(['nova', 'director'])
    // Signing in or moving between pages loses the query; the visit still knows.
    expect(initClientEmbed({ search: '', storage, themes })).toEqual({ client: 'nova', theme: 'director' })
    expect(JSON.parse(storage.items.get(EMBED_STORAGE_KEY))).toEqual({ client: 'nova', theme: 'director' })
  })

  it('is a plain console for any other client, no client, or a theme it does not know', () => {
    expect(initClientEmbed({ search: '?client=someone&theme=director', storage: memoryStorage(), themes })).toBeNull()
    expect(embeddedClient()).toBe('')
    expect(initClientEmbed({ search: '?theme=director', storage: memoryStorage(), themes })).toBeNull()
    expect(initClientEmbed({ search: '?client=nova&theme=crimson', storage: memoryStorage(), themes })).toEqual({ client: 'nova', theme: '' })
    const tampered = memoryStorage()
    tampered.setItem(EMBED_STORAGE_KEY, '{"client":"elsewhere","theme":"oled"}')
    expect(initClientEmbed({ search: '', storage: tampered, themes })).toBeNull()
    const broken = memoryStorage()
    broken.setItem(EMBED_STORAGE_KEY, '{not json')
    expect(initClientEmbed({ search: '', storage: broken, themes })).toBeNull()
  })

  it('works without session storage, for the page it is on', () => {
    const blocked = { getItem() { throw new Error('blocked') }, setItem() { throw new Error('blocked') } }
    expect(initClientEmbed({ search: '?client=nova&theme=oled', storage: blocked, themes })).toEqual({ client: 'nova', theme: 'oled' })
    expect(embeddedTheme()).toBe('oled')
  })

  it('lets a theme picked in the console replace Nova\'s for the rest of the visit', () => {
    const storage = memoryStorage()
    initClientEmbed({ search: '?client=nova&theme=director', storage, themes })
    forgetEmbeddedTheme(storage)
    expect([embeddedClient(), embeddedTheme()]).toEqual(['nova', ''])
    expect(initClientEmbed({ search: '', storage, themes })).toEqual({ client: 'nova', theme: '' })
  })

  it('builds Nova\'s launch link with every value encoded', () => {
    expect(artLaunchHref({
      hostUuid: '1f0e8b6a-3c1b-4f3e-9d2a-8b7c6d5e4f30', hostName: 'Living Room PC',
      app: { uuid: 'c0ffee00-0000-4000-8000-000000000001', name: 'Ratchet & Clank: Rift Apart #1' },
    })).toBe('art://launch?host_uuid=1f0e8b6a-3c1b-4f3e-9d2a-8b7c6d5e4f30&host_name=Living%20Room%20PC'
      + '&app_uuid=c0ffee00-0000-4000-8000-000000000001&app_name=Ratchet%20%26%20Clank%3A%20Rift%20Apart%20%231')
    expect(artLaunchHref({ hostUuid: '', hostName: undefined, app: { uuid: 'x' } })).toBe('art://launch?host_uuid=&host_name=&app_uuid=x&app_name=')
  })
})
