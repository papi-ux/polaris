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
    expect(copy.vaapi_quality_desc).toContain('costs no extra encode time')
    expect(copy.vaapi_strict_rc_buffer_desc).toContain('On AMD the limit holds only in CBR, so auto uses CBR with it there')
  })
})
