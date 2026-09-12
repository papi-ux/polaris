import { describe, expect, it } from 'vitest'

import { groupRecentIssueLogs } from './recent-issues.js'

const start = '[2026-08-16 10:00:00.000]: Info: // Testing for available encoders, this may generate errors. You can safely ignore those errors. //'
const end = '[2026-08-16 10:00:01.000]: Info: // Ignore any errors mentioned above, they are not relevant. //'

describe('recent issue log grouping', () => {
  it('suppresses expected errors only inside encoder probe windows', () => {
    const codecError = 'Error: Could not open codec [hevc_vaapi]'
    const grouped = groupRecentIssueLogs([
      '[2026-08-16 09:59:59.000]: Warning: real warning before probe',
      start,
      `[2026-08-16 10:00:00.100]: ${codecError}`,
      '[2026-08-16 10:00:00.200]: Error: shader probe failed',
      end,
      `[2026-08-16 10:00:02.000]: ${codecError}`,
    ].join('\n'))

    expect(grouped).toEqual([
      expect.objectContaining({ level: 'Error', message: 'Could not open codec [hevc_vaapi]', count: 1 }),
      expect.objectContaining({ level: 'Warning', message: 'real warning before probe', count: 1 }),
    ])
    expect(grouped.some((entry) => entry.message === 'shader probe failed')).toBe(false)
  })

  it('keeps a driver too old for this build visible inside a probe window', () => {
    const grouped = groupRecentIssueLogs([
      start,
      '[2026-08-16 10:00:00.100]: Error: [h264_nvenc @ 0x1] Driver does not support the required nvenc API version. Required: 13.1 Found: 13.0',
      '[2026-08-16 10:00:00.110]: Error: [h264_nvenc @ 0x1] The minimum required Nvidia driver for nvenc is 610.00 or newer',
      '[2026-08-16 10:00:00.120]: Error: Could not open codec [h264_nvenc]: Function not implemented',
      end,
    ].join('\n'))

    expect(grouped.map((entry) => entry.message)).toEqual([
      '[h264_nvenc @ 0x1] The minimum required Nvidia driver for nvenc is 610.00 or newer',
      '[h264_nvenc @ 0x1] Driver does not support the required nvenc API version. Required: 13.1 Found: 13.0',
    ])
  })

  it('recognizes a bounded tail that begins inside a probe window', () => {
    const grouped = groupRecentIssueLogs([
      '[2026-08-16 10:00:00.100]: Error: expected leading probe failure',
      end,
      '[2026-08-16 10:00:02.000]: Error: live stream failure',
    ].join('\n'))

    expect(grouped).toEqual([
      expect.objectContaining({ message: 'live stream failure' }),
    ])
  })

  it('groups matching non-probe issues and keeps the newest timestamp', () => {
    const grouped = groupRecentIssueLogs([
      '[2026-08-16 10:00:00.000]: Warning: connection stalled',
      '[2026-08-16 10:00:01.000]: Warning: connection   stalled',
    ].join('\n'))

    expect(grouped).toEqual([
      {
        timestamp: '[2026-08-16 10:00:01.000]',
        level: 'Warning',
        message: 'connection stalled',
        count: 2,
      },
    ])
  })
})
