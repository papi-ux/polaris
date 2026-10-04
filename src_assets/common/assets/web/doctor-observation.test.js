import { describe, expect, it } from 'vitest'
import { doctorIsWaitingForStream } from './doctor-observation.js'

describe('Doctor idle observation', () => {
  const idle = { primary_issue: 'no_active_stream', status: 'unknown', severity: 'info' }
  it('only quiets an explicitly informational idle verdict', () => {
    expect(doctorIsWaitingForStream(idle, { streaming: false })).toBe(true)
    for (const doctor of [
      { ...idle, severity: 'warning' },
      { ...idle, primary_issue: 'stream_failed_to_start' },
      { ...idle, primary_issue: 'capture_missing' },
      { ...idle, primary_issue: 'capture_configuration' },
    ]) expect(doctorIsWaitingForStream(doctor, { streaming: false })).toBe(false)
    expect(doctorIsWaitingForStream(idle, { streaming: true })).toBe(false)
    expect(doctorIsWaitingForStream(idle, {})).toBe(false)
    expect(doctorIsWaitingForStream(null, { streaming: false })).toBe(false)
  })
})
