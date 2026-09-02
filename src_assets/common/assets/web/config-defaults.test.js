import { readFileSync } from 'node:fs'
import { join } from 'node:path'

// The launch-mode and planner copy moved into the locale file (config.av_*),
// so copy pins read the en.json strings while wiring pins stay on the source.
const avLocaleCopy = () => {
  const locale = JSON.parse(readFileSync(
    join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'),
    'utf8',
  ))
  return Object.entries(locale.config)
    .filter(([key]) => key.startsWith('av_'))
    .map(([, value]) => value)
    .join('\n')
}

describe('config defaults', () => {
  it('presents Private Stream as the default and GPU-native as capture capability/status', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/configs/tabs/AudioVideo.vue'), 'utf8')
    const avCopy = avLocaleCopy()
    const launchModeSurface = `${source}\n${avCopy}`

    // Mode titles now live in en.json; the source must still wire their keys.
    expect(source).toContain('config.av_mode_headless_stream_title')
    expect(source).toContain('config.av_mode_windowed_stream_title')
    expect(source).toContain('config.av_mode_desktop_display_title')
    expect(avCopy).toContain('Private Stream')
    expect(avCopy).toContain('Private Stream (GPU-native)')
    expect(avCopy).toContain('Mirror Desktop')
    // Stays in the source: the GPU path explainer tile is not part of the moved copy.
    expect(source).toContain('GPU-native is a labwc capture preference')
    // Checklist copy moved with the rest of the checklist strings.
    expect(avCopy).toContain('session health shows SHM/system-memory fallback')
    // Banned vocabulary must stay off the whole surface: source plus locale copy.
    expect(launchModeSurface).not.toContain('GPU-Native Test')
    expect(launchModeSurface).not.toContain('Experimental')
    expect(launchModeSurface).not.toContain('GPU-Native Stream')
    expect(launchModeSurface).not.toContain('Desktop Display')
    expect(launchModeSurface).not.toContain('capable NVIDIA/Wayland hosts')
    expect(launchModeSurface).not.toContain('DMA-BUF/CUDA/NVENC hosts')
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
    const avCopy = avLocaleCopy()

    expect(source).toContain('data-display-resolution-planner')
    expect(source).toContain('displayPlanner.recommendedTitle')
    expect(source).toContain('data-display-resolution-planner-advanced')
    expect(source).toContain('showDisplayPlannerAdvanced')
    // Planner prose moved to en.json; the source must wire the keys and the
    // phrases must survive in the locale copy.
    expect(source).toContain('config.av_planner_moonlight_title')
    expect(source).toContain('config.av_planner_advanced_copy')
    expect(avCopy).toContain('Moonlight compatibility stays standard')
    expect(avCopy).toContain('Scale factors are capped to 0.5x–2x')
    expect(avCopy).toContain('client/game needs a specific override')
  })
})
