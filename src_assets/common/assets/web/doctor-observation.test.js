import { describe, expect, it } from 'vitest'
import { doctorIsWaitingForStream, idleHostFindings } from './doctor-observation.js'

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

  it('keeps only the host configuration cards of a checklist, in their order', () => {
    // Before a stream the live rows have nothing to measure; a host finding is current whatever
    // streams, so it is the one kind of card an idle Doctor shows.
    const row = (key, status = 'info') => ({ key, label: key, status, detail: key, action: key })
    const checklist = [
      row('connection'), row('host-config', 'warning'), row('host-config-capture_set_aside_by_mode'),
      row('display-mode', 'warning'), row('packet-loss'), row('capture-path'), row('encoder-pressure'),
      row('auth-pairing', 'pass'), row('logs', 'pass'),
    ]
    expect(idleHostFindings(checklist).map((item) => item.key))
      .toEqual(['host-config', 'host-config-capture_set_aside_by_mode'])
    expect(idleHostFindings(checklist)[0]).toBe(checklist[1])
    expect(idleHostFindings([row('host-configuration'), row('connection')])).toEqual([])
    for (const value of [undefined, null, {}, 'host-config', 0]) expect(idleHostFindings(value)).toEqual([])
  })
})
