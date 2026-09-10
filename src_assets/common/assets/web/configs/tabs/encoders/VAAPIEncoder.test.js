import { mount } from '@vue/test-utils'
import VAAPIEncoder from './VAAPIEncoder.vue'

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
})
