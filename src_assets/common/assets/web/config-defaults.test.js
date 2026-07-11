import { readFileSync } from 'node:fs'
import { join } from 'node:path'

describe('config defaults', () => {
  it('presents Private Stream as the default and GPU-native as capture capability/status', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/configs/tabs/AudioVideo.vue'), 'utf8')

    expect(source).toContain('Private Stream')
    expect(source).toContain('Private Stream (GPU-native)')
    expect(source).toContain('GPU-native appears in session health as the capture path')
    expect(source).toContain('Mirror Desktop')
    expect(source).not.toContain('GPU-Native Test')
    expect(source).not.toContain('Experimental')
    expect(source).not.toContain('GPU-Native Stream')
    expect(source).not.toContain('Desktop Display')
    expect(source).not.toContain('capable NVIDIA/Wayland hosts')
    expect(source).not.toContain('DMA-BUF/CUDA/NVENC hosts')
  })

  it('uses dynamic stream-display runtime notices instead of static restart warnings', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/configs/tabs/AudioVideo.vue'), 'utf8')

    expect(source).toContain('resolveStreamDisplayRuntimeNotice')
    expect(source).toContain('streamDisplayRuntimeNotice.copy')
    expect(source).not.toContain('restartCopy')
    expect(source).not.toContain('Requires restart.')
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

  it('surfaces the display planner as simple recommendations with advanced scale controls tucked away', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/configs/tabs/AudioVideo.vue'), 'utf8')

    expect(source).toContain('data-display-resolution-planner')
    expect(source).toContain('displayPlanner.recommendedTitle')
    expect(source).toContain('Moonlight compatibility stays standard')
    expect(source).toContain('data-display-resolution-planner-advanced')
    expect(source).toContain('showDisplayPlannerAdvanced')
    expect(source).toContain('Scale factors are capped to 0.5x–2x')
    expect(source).toContain('client/game needs a specific override')
  })
})
