import { describe, expect, it } from 'vitest'

import { previewOutputForConfig } from './dashboard-preview-output.js'

describe('dashboard preview output', () => {
  it('crops to the saved connector only in modes that stream it', () => {
    expect(previewOutputForConfig({ linux_stream_mode: 'headless_dongle', linux_streaming_output: 'HDMI-A-2' })).toBe('HDMI-A-2')
    expect(previewOutputForConfig({ linux_stream_mode: 'host_virtual_display', linux_streaming_output: 'DP-1' })).toBe('DP-1')
  })

  it('never crops a private or mirrored session to a connector the file keeps for later (#633)', () => {
    for (const mode of ['headless_stream', 'windowed_stream', 'desktop_display', 'gamescope_stream', '']) {
      expect(previewOutputForConfig({ linux_stream_mode: mode, linux_streaming_output: 'DP-1' }), mode).toBe('')
    }
  })

  it('still honors an explicit capture Output Name in any mode', () => {
    expect(previewOutputForConfig({ linux_stream_mode: 'headless_stream', linux_streaming_output: 'DP-1', output_name: 'HEADLESS-1' })).toBe('HEADLESS-1')
    expect(previewOutputForConfig({})).toBe('')
  })
})
