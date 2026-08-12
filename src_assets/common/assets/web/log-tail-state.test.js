import { Buffer } from 'node:buffer'
import { describe, expect, it, vi } from 'vitest'

import {
  applyLogTailPayload,
  createLogTailState,
  fetchLogTail,
  MAX_BROWSER_LOG_BYTES,
  MAX_BROWSER_LOG_LINES,
} from './log-tail-state.js'

function payload(content, overrides = {}) {
  const bytes = Buffer.from(content)
  const startOffset = overrides.start_offset ?? 0
  return {
    status: true,
    schema_version: 1,
    content_encoding: 'base64',
    content: bytes.toString('base64'),
    content_bytes: bytes.length,
    media_type: 'text/plain',
    charset: 'utf-8',
    start_offset: startOffset,
    end_offset: overrides.end_offset ?? startOffset + bytes.length,
    generation: 7,
    truncated: false,
    reset: true,
    ...overrides,
  }
}

describe('bounded browser log-tail state', () => {
  it('starts empty and bounded', () => {
    const state = createLogTailState()

    expect(state.text).toBe('')
    expect(state.bytes).toBeInstanceOf(Uint8Array)
    expect(state.bytes).toHaveLength(0)
    expect(state.startOffset).toBeNull()
    expect(state.endOffset).toBeNull()
    expect(state.generation).toBeNull()
  })

  it('replaces state on an initial response and preserves truncation metadata', () => {
    const next = applyLogTailPayload(createLogTailState(), payload('recent\n', {
      start_offset: 100,
      end_offset: 107,
      truncated: true,
      reset: true,
    }))

    expect(next.text).toBe('recent\n')
    expect(next.startOffset).toBe(100)
    expect(next.endOffset).toBe(107)
    expect(next.generation).toBe(7)
    expect(next.truncated).toBe(true)
    expect(next.reset).toBe(true)
  })

  it('appends only a contiguous delta', () => {
    const initial = applyLogTailPayload(createLogTailState(), payload('one\n', {
      end_offset: 4,
    }))
    const next = applyLogTailPayload(initial, payload('two\n', {
      start_offset: 4,
      end_offset: 8,
      reset: false,
    }))

    expect(next.text).toBe('one\ntwo\n')
    expect(next.startOffset).toBe(0)
    expect(next.endOffset).toBe(8)
    expect(next.reset).toBe(false)
  })

  it('fails closed to replacement when a claimed delta is discontinuous', () => {
    const initial = applyLogTailPayload(createLogTailState(), payload('old\n', {
      start_offset: 10,
      end_offset: 14,
    }))
    const next = applyLogTailPayload(initial, payload('new\n', {
      start_offset: 20,
      end_offset: 24,
      reset: false,
    }))

    expect(next.text).toBe('new\n')
    expect(next.startOffset).toBe(20)
    expect(next.endOffset).toBe(24)
    expect(next.reset).toBe(true)
  })

  it('replaces a prior window when the server reports a reset', () => {
    const initial = applyLogTailPayload(createLogTailState(), payload('old\n', {
      start_offset: 10,
      end_offset: 14,
    }))
    const next = applyLogTailPayload(initial, payload('new\n', {
      start_offset: 14,
      end_offset: 18,
      reset: true,
    }))

    expect(next.text).toBe('new\n')
    expect(next.startOffset).toBe(14)
    expect(next.endOffset).toBe(18)
    expect(next.reset).toBe(true)
  })

  it('replaces a numerically contiguous window from another generation', () => {
    const initial = applyLogTailPayload(createLogTailState(), payload('old\n', {
      start_offset: 10,
      end_offset: 14,
    }))
    const next = applyLogTailPayload(initial, payload('new\n', {
      start_offset: 14,
      end_offset: 18,
      generation: 8,
      reset: false,
    }))

    expect(next.text).toBe('new\n')
    expect(next.generation).toBe(8)
    expect(next.reset).toBe(true)
  })

  it('bounds accumulated bytes and lines after incremental appends', () => {
    let state = createLogTailState()
    let offset = 0
    for (let index = 0; index < 20; ++index) {
      const line = `line-${String(index).padStart(2, '0')}\n`
      state = applyLogTailPayload(state, payload(line, {
        start_offset: offset,
        end_offset: offset + line.length,
        reset: index === 0,
      }), {
        maxBytes: 64,
        maxLines: 4,
      })
      offset += line.length
    }

    expect(state.bytes.byteLength).toBeLessThanOrEqual(64)
    expect(state.text.trim().split('\n')).toHaveLength(4)
    expect(state.text).toContain('line-19')
    expect(state.text).not.toContain('line-15')
    expect(state.truncated).toBe(true)
  })

  it('keeps a large response inside the production browser bound', () => {
    const large = `${'x'.repeat(1023)}\n`.repeat(1024)
    const state = applyLogTailPayload(createLogTailState(), payload(large), {
      maxBytes: MAX_BROWSER_LOG_BYTES,
      maxLines: MAX_BROWSER_LOG_LINES,
    })

    expect(state.bytes.byteLength).toBeLessThanOrEqual(MAX_BROWSER_LOG_BYTES)
    expect(state.text.split('\n').length - 1).toBeLessThanOrEqual(MAX_BROWSER_LOG_LINES)
    expect(state.truncated).toBe(true)
  })

  it('rejects malformed or internally inconsistent payloads', () => {
    const state = createLogTailState()

    expect(() => applyLogTailPayload(state, payload('bad', { schema_version: 2 }))).toThrow(/schema/i)
    expect(() => applyLogTailPayload(state, payload('bad', { content_encoding: 'utf-8' }))).toThrow(/encoding/i)
    expect(() => applyLogTailPayload(state, payload('bad', { content_bytes: 99 }))).toThrow(/byte count/i)
    expect(() => applyLogTailPayload(state, payload('bad', { end_offset: 99 }))).toThrow(/offset/i)
    expect(() => applyLogTailPayload(state, payload('bad', { generation: -1 }))).toThrow(/generation/i)
    expect(() => applyLogTailPayload(state, payload('bad', { content: 'not base64!' }))).toThrow(/base64/i)
  })

  it('fetches the versioned endpoint with bounds and a prior cursor', async () => {
    const fetchImpl = vi.fn().mockResolvedValue({
      ok: true,
      json: async () => payload('delta\n', {
        start_offset: 42,
        end_offset: 48,
        reset: false,
      }),
    })
    const state = {
      ...createLogTailState(),
      bytes: new TextEncoder().encode('prior'),
      text: 'prior',
      startOffset: 37,
      endOffset: 42,
      generation: 7,
    }

    const next = await fetchLogTail(state, { fetchImpl })

    expect(fetchImpl).toHaveBeenCalledOnce()
    const [url, options] = fetchImpl.mock.calls[0]
    expect(url).toContain('/polaris/v1/diagnostics/logs/tail?')
    expect(url).toContain(`max_bytes=${MAX_BROWSER_LOG_BYTES}`)
    expect(url).toContain(`max_lines=${MAX_BROWSER_LOG_LINES}`)
    expect(url).toContain('after=42')
    expect(url).toContain('after_generation=7')
    expect(options).toMatchObject({ credentials: 'include', cache: 'no-store' })
    expect(next.text).toBe('priordelta\n')
  })
})
