import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { describe, expect, it } from 'vitest'

describe('paired client timestamps', () => {
  it('formats Unix seconds as a local absolute date and time', async () => {
    const timestamps = await import('./client-timestamps.js').catch(() => ({}))

    expect(timestamps.formatClientTimestamp).toBeTypeOf('function')
    expect(timestamps.formatClientTimestamp(1_720_000_000, 'en-US', 'UTC')).toBe('Jul 3, 2024, 9:46 AM')
  })

  it('normalizes Unix milliseconds instead of rendering a far-future year', async () => {
    const timestamps = await import('./client-timestamps.js').catch(() => ({}))

    expect(timestamps.formatClientTimestamp(1_720_000_000_000, 'en-US', 'UTC')).toBe('Jul 3, 2024, 9:46 AM')
  })

  it('does not invent dates for legacy or invalid values', async () => {
    const timestamps = await import('./client-timestamps.js').catch(() => ({}))

    expect(timestamps.formatClientTimestamp).toBeTypeOf('function')
    expect(timestamps.formatClientTimestamp(0, 'en-US', 'UTC')).toBe('Not recorded')
    expect(timestamps.formatClientTimestamp(undefined, 'en-US', 'UTC')).toBe('Not recorded')
    expect(timestamps.formatClientTimestamp('garbage', 'en-US', 'UTC')).toBe('Not recorded')
  })

  it('uses the caller-provided localized fallback for missing values', async () => {
    const timestamps = await import('./client-timestamps.js').catch(() => ({}))

    expect(timestamps.formatClientTimestamp(0, 'es-US', 'UTC', 'Sin registrar')).toBe('Sin registrar')
  })

  it('renders Added and Last seen values on saved-device cards', () => {
    const source = readFileSync(resolve(process.cwd(), 'src_assets/common/assets/web/views/PinView.vue'), 'utf8')

    expect(source).toContain("$t('pin.date_added')")
    expect(source).toContain("$t('pin.last_seen')")
    expect(source).toContain("formatClientTimestamp(client.paired_at, undefined, undefined, $t('pin.not_recorded'))")
    expect(source).toContain("formatClientTimestamp(client.last_seen_at, undefined, undefined, $t('pin.not_recorded'))")
  })
})
