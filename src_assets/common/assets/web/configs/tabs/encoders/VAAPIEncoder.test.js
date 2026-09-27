import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { mount } from '@vue/test-utils'
import VAAPIEncoder from './VAAPIEncoder.vue'

const locale = () => JSON.parse(readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'),
  'utf8',
)).config

describe('VA-API session settings', () => {
  it('preserves automatic defaults until the user selects explicit controls', async () => {
    const config = { vaapi_quality: 'auto', vaapi_rc: 'auto', vaapi_blbrc: 'auto', vaapi_strict_rc_buffer: 'disabled' }
    const wrapper = mount(VAAPIEncoder, {
      props: { config, platform: 'linux' },
      global: { mocks: { $t: key => key }, stubs: { Checkbox: true } },
    })
    for (const key of ['vaapi_quality', 'vaapi_rc', 'vaapi_blbrc']) {
      expect(wrapper.get(`#${key}`).element.value).toBe('auto')
    }
    await wrapper.get('#vaapi_quality').setValue('balanced')
    await wrapper.get('#vaapi_rc').setValue('qvbr')
    await wrapper.get('#vaapi_blbrc').setValue('enabled')
    expect(config).toEqual({ vaapi_quality: 'balanced', vaapi_rc: 'qvbr', vaapi_blbrc: 'enabled', vaapi_strict_rc_buffer: 'disabled' })
    await wrapper.get('#vaapi_rc').setValue('auto')
    expect(config.vaapi_rc).toBe('auto')
  })

  it('says which way each quality preset moves encode latency', () => {
    const copy = locale()
    expect(copy.vaapi_quality_speed).toBe('Prefer speed (lowest latency)')
    expect(copy.vaapi_quality_balanced).toBe('Balanced')
    expect(copy.vaapi_quality_quality).toBe('Prefer quality (more encode time)')
    expect(copy.vaapi_quality_desc).toContain('adds that time to stream latency')
    // Balanced was measured once, on VCN 4, so the copy says where rather than promising it everywhere.
    expect(copy.vaapi_quality_desc).toContain('at no measurable cost on VCN 4')
    expect(copy.vaapi_quality_desc).not.toContain('no extra encode time')
    // The slower presets were timed only on that VCN 4 card too, so the reason they go unused names it.
    expect(copy.vaapi_quality_desc).toContain('because on VCN 4 they were too slow for a 4K AV1 stream at 60 fps')
    expect(copy.vaapi_quality_desc).not.toContain('cannot keep up')
    expect(copy.vaapi_strict_rc_buffer_desc).toContain('the limit holds only in CBR, so auto uses CBR with it there')
  })

  it('names Mesa radeonsi, the driver the AMD quality bits and CBR pairing are gated on', () => {
    const copy = locale()
    const tuning = readFileSync(join(process.cwd(), 'src/platform/linux/vaapi_tuning.h'), 'utf8')
    // The code applies both only when the VA-API driver is radeonsi, not on every AMD GPU.
    expect(tuning).toContain('return is_driver(vendor) && range >= static_cast<uint32_t>(quality);')
    expect(tuning).toContain('!(radeonsi::is_driver(vendor) && (mask & VA_RC_CBR))')
    expect(copy.vaapi_quality_desc).toContain('On AMD (Mesa radeonsi), Prefer speed')
    expect(copy.vaapi_strict_rc_buffer_desc).toContain('On AMD (Mesa radeonsi) the limit holds only in CBR')
    for (const key of ['vaapi_quality_desc', 'vaapi_strict_rc_buffer_desc']) {
      expect(copy[key]).not.toMatch(/On AMD(?! \(Mesa radeonsi\))/)
    }
  })
})
