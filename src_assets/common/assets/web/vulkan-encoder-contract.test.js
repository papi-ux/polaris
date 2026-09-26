import { readFileSync } from 'node:fs'
import { join } from 'node:path'

const webSource = (relativePath) => readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web', relativePath),
  'utf8',
)

const source = (relativePath) => readFileSync(
  join(process.cwd(), relativePath),
  'utf8',
)

describe('Vulkan Video settings contract', () => {
  it('describes explicit Vulkan and the live-validated AMD Auto policy', () => {
    const advanced = webSource('configs/tabs/Advanced.vue')
    const encoder = webSource('configs/tabs/encoders/VulkanEncoder.vue')

    expect(advanced).toContain('<option value="vulkan">Vulkan Video (experimental)</option>')
    expect(encoder).toContain('Auto can prefer Vulkan Video on a compatible AMD private-stream route')
    expect(encoder).toContain("NVIDIA's proprietary driver remains on NVENC, Nouveau uses capability probing, and Intel remains on VA-API by default")
    expect(encoder).toContain('supports DRM/KMS, wlroots, and Portal capture')
    expect(encoder).toContain('Explicit Vulkan selection is strict')
    expect(encoder).toContain('H.264 and HEVC are enabled; AV1 remains unavailable')
    expect(encoder).toContain('Doctor reports the detected driver')
    expect(encoder).toContain('id="vk_tune" class="settings-input"')
    expect(encoder).toContain('id="vk_rc_mode" class="settings-input"')
    expect(encoder).toContain('id="vk_quality" class="settings-input"')
  })

  it('offers only the vk_quality levels the probed driver exposes (0 through max-1)', () => {
    const encoder = webSource('configs/tabs/encoders/VulkanEncoder.vue')
    const locale = JSON.parse(webSource('public/assets/locale/en.json')).config

    expect(locale.vk_quality_default).toBe('Level 0 (default, fastest encode)')
    // Every level above 0 costs encode time on each frame, and the labels say so.
    expect(locale.vk_quality_level).toBe('Quality level {level} (more encode time)')
    expect(locale.vk_quality_desc).toContain('adds that time to stream latency')
    expect(locale.vk_quality_desc).toContain("one less than the driver's reported maximum")
    // The description must explain that Polaris clamps out-of-range saved values.
    expect(locale.vk_quality_desc).toContain('clamped to that maximum when the session starts, with a warning logged')

    // Vulkan requires qualityLevel < maxQualityLevels; the probed driver's count decides
    // how many levels are offered (vk_quality_max = maxQualityLevels-1), and level 0 is always present.
    expect(encoder).toMatch(/const vkQualityMax = computed\(\(\) => \{[\s\S]*?encoder_codec_support\?\.vk_quality_max[\s\S]*?\}\)/)
    const qualitySelect = encoder.match(/<select id="vk_quality"[\s\S]*?<\/select>/)?.[0] ?? ''
    expect(qualitySelect).toContain('<option value="0">')
    expect(qualitySelect).toMatch(/v-for="level in vkQualityMax"/)
    expect(qualitySelect).not.toMatch(/value="[1-9]"/)

    // A saved level above the probed maximum (or saved before any probe ran) keeps its own
    // option so the select doesn't read blank; Polaris clamps it when the session starts.
    expect(locale.vk_quality_unsupported).toContain('{level}')
    expect(encoder).toMatch(/const vkQualitySaved = computed\(\(\) => \{[\s\S]*?vkQualityMax\.value[\s\S]*?\}\)/)
    expect(qualitySelect).toContain('v-if="vkQualitySaved"')
  })

  it('serves the probed Vulkan quality maximum from encoder_codec_support', () => {
    const videoHeader = source('src/video.h')
    const configHttp = source('src/confighttp.cpp')

    expect(videoHeader).toContain('int advertised_vulkan_quality_max();')
    expect(configHttp).toMatch(/const int vk_quality_max = video::advertised_vulkan_quality_max\(\);/)
    expect(configHttp).toContain('{"vk_quality_max", vk_quality_max >= 0 ? nlohmann::json(vk_quality_max) : nlohmann::json(nullptr)},')
  })

  it('registers low-latency CBR defaults and renders the encoder panel', () => {
    const configView = webSource('views/ConfigView.vue')
    const container = webSource('configs/tabs/ContainerEncoders.vue')

    expect(configView).toMatch(/id: "vulkan",[\s\S]*"vk_tune": 2,[\s\S]*"vk_rc_mode": 2/)
    expect(container).toContain("import VulkanEncoder from './encoders/VulkanEncoder.vue'")
    expect(container).toContain('v-if="currentTab === \'vulkan\'"')
  })

  it('shows automatic encoder selection evidence in Doctor', () => {
    const troubleshooting = webSource('views/TroubleshootingView.vue')
    const locale = JSON.parse(webSource('public/assets/locale/en.json')).troubleshooting

    expect(troubleshooting).toContain('doctor.advanced_evidence?.encoder_selection')
    for (const [key, label] of Object.entries({
      doctor_detected_gpu_driver: 'Detected GPU driver',
      doctor_encoder_policy: 'Encoder policy',
      doctor_preferred_encoder: 'Preferred encoder',
      doctor_fallback_encoder: 'Fallback encoder',
      doctor_fallback_used: 'Fallback used',
      doctor_selection_reason: 'Selection reason',
    })) {
      expect(troubleshooting).toContain(`i18n.t('troubleshooting.${key}')`)
      expect(locale[key]).toBe(label)
    }
  })

  it('promotes Vulkan only for AMD private Auto and preserves strict fallback behavior', () => {
    const video = source('src/video.cpp')
    const portal = source('src/platform/linux/portal_grab.cpp')
    const policy = source('src/platform/linux/encoder_auto_policy.h')
    const vulkan = source('src/platform/linux/vulkan_encode.cpp')

    expect(policy).toMatch(/kernel_driver == "amdgpu" && private_compositor_live_probe_available[\s\S]*prefer_vulkan = true/)
    expect(policy).toMatch(/kernel_driver == "nvidia"[\s\S]*preferred_encoder = "nvenc"/)
    expect(policy).toMatch(/kernel_driver == "nouveau"[\s\S]*policy = "nouveau_availability_probe"[\s\S]*preferred_encoder = "automatic"/)
    expect(video).toMatch(/selection_plan\.policy == "amd_private_vulkan_live_probe"[\s\S]*std::erase\(encoder_list, &vulkan\)/)
    expect(video).toContain('strict_configured_encoder || config::video.encoder == "vulkan"sv')
    expect(video).toMatch(/LIMITED_GOP_SIZE \| PARALLEL_ENCODING \| NO_AV1/)
    expect(video).not.toContain('release_encode_resources')
    expect(video).toMatch(/avcodec_ctx\.reset\(\);[\s\S]*converter\.reset\(\);/)
    expect(portal).toContain('make_avcodec_encode_device_ram')
    expect(video).toContain('automatic_encoder_prefers_gpu_native_capture')
    expect(vulkan).toMatch(/target\.initialized = false;[\s\S]*hwframe\.reset\(\);[\s\S]*resources_released = true;/)
  })
})
