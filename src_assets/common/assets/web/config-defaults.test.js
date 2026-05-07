import { readFileSync } from 'node:fs'
import { join } from 'node:path'

describe('config defaults', () => {
  it('keeps GPU-native capture disabled by default so enabling it persists', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/ConfigView.vue'), 'utf8')

    expect(source).toContain('"linux_prefer_gpu_native_capture": "disabled"')
  })

  it('uses Browser Stream as the primary browser streaming config key', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/ConfigView.vue'), 'utf8')

    expect(source).toContain('"browser_streaming": "disabled"')
    expect(source).not.toContain('"webrtc_browser_streaming": "disabled"')
  })
})
