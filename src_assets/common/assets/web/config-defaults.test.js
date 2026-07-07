import { readFileSync } from 'node:fs'
import { join } from 'node:path'

describe('config defaults', () => {
  it('presents GPU-native capture as a primary stream mode with vendor-neutral fallback copy', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/configs/tabs/AudioVideo.vue'), 'utf8')

    expect(source).toContain('GPU-Native Stream')
    expect(source).toContain('may still fall back to Headless Stream with SHM/RAM capture')
    expect(source).toContain('capable GPU-native hosts')
    expect(source).not.toContain('GPU-Native Test')
    expect(source).not.toContain('Experimental')
    expect(source).not.toContain('capable NVIDIA/Wayland hosts')
    expect(source).not.toContain('DMA-BUF/CUDA/NVENC hosts')
  })

  it('uses Browser Stream as the primary browser streaming config key', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/ConfigView.vue'), 'utf8')

    expect(source).toContain('"browser_streaming": "disabled"')
    expect(source).not.toContain('"webrtc_browser_streaming": "disabled"')
  })

  it('keeps NVENC split-frame encoding disabled by default', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/ConfigView.vue'), 'utf8')

    expect(source).toContain('"nvenc_split_encode_mode": "disabled"')
  })
})
