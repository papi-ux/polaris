import { describe, expect, it } from 'vitest'
import {
  PREVIEW_MAX_BACKOFF_MS,
  nextPreviewBackoffMs,
  shouldPausePreviewPolling,
} from './display-preview-state'

describe('display preview state helpers', () => {
  it('backs off failed preview refreshes without exceeding the maximum delay', () => {
    expect(nextPreviewBackoffMs(2000)).toBe(15000)
    expect(nextPreviewBackoffMs(15000)).toBe(30000)
    expect(nextPreviewBackoffMs(30000)).toBe(PREVIEW_MAX_BACKOFF_MS)
    expect(nextPreviewBackoffMs(PREVIEW_MAX_BACKOFF_MS)).toBe(PREVIEW_MAX_BACKOFF_MS)
  })

  it('pauses preview polling after repeated failures', () => {
    expect(shouldPausePreviewPolling(0)).toBe(false)
    expect(shouldPausePreviewPolling(2)).toBe(false)
    expect(shouldPausePreviewPolling(3)).toBe(true)
  })
})
