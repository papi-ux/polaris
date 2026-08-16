import { describe, expect, it } from 'vitest'

import { presentVirtualDisplayStatus } from './virtual-display-status.js'

describe('virtual display status presentation', () => {
  it('explains that Private Stream intentionally bypasses host virtual displays', () => {
    const state = presentVirtualDisplayStatus({
      policy_mode: 'headless_stream',
      available: false,
      backend_detected: false,
    })

    expect(state.kind).toBe('unused')
    expect(state.label).toBe('Not needed for Private Stream')
  })

  it('shows backend-specific configuration guidance when kscreen-doctor is detected', () => {
    const state = presentVirtualDisplayStatus({
      policy_mode: 'host_virtual_display',
      available: false,
      backend_detected: true,
      backend: 'kscreen-doctor',
      unavailable_reason: 'kscreen-doctor backend needs linux_streaming_output set to the output it may reconfigure.',
    })

    expect(state.kind).toBe('unconfigured')
    expect(state.detail).toContain('linux_streaming_output')
  })

  it('reserves dependency guidance for a genuinely missing backend', () => {
    expect(presentVirtualDisplayStatus({
      policy_mode: 'host_virtual_display',
      available: false,
      backend_detected: false,
    }).kind).toBe('missing')
  })
})
