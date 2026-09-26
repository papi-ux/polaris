import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'
import DashboardSpaces from './DashboardSpaces.vue'
import { summarizeDashboardSpaces } from '../dashboard-spaces.js'
import { spacesGlobal } from './spaces-test-i18n.js'

const state = { enabled: true, available: true, profiles: [{ id: 'a', name: 'Alex’s Space', clients: [], family: 'heroic' }],
  activity: [{ profile_id: 'a', client_id: 'private-client-id', state: 'running' }] }
function render(extra = {}) {
  return mount(DashboardSpaces, { props: { model: summarizeDashboardSpaces(state, { loaded: true, ...extra }) },
    global: { ...spacesGlobal, stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' } } } })
}
describe('Spaces session card', () => {
  it('lists Space and launcher without presenting host capture metrics or device identifiers', () => {
    const wrapper = render()
    expect(wrapper.text()).toContain('Alex’s Space')
    expect(wrapper.text()).toContain('Heroic')
    expect(wrapper.text()).toContain('Unrecognized device')
    expect(wrapper.text()).not.toContain('private-client-id')
    expect(wrapper.text()).toContain('Desktop preview is paused')
    expect(wrapper.find('img').exists()).toBe(false)
    expect(wrapper.get('a').attributes('href')).toBe('/spaces')
    wrapper.unmount()
  })
  it('marks retained sessions stale after a failed refresh', () => {
    const wrapper = render({ error: true })
    expect(wrapper.get('[data-spaces-attention]').text()).toBe('Needs attention')
    expect(wrapper.get('[data-space-session]').text()).toContain('Last reported: running')
    expect(wrapper.get('[role="alert"]').text()).toContain('may be out of date')
    wrapper.unmount()
  })
  it('offers a read-only refresh and prevents repeat clicks while loading', async () => {
    const wrapper = render()
    await wrapper.get('button').trigger('click')
    expect(wrapper.emitted('refresh')).toHaveLength(1)
    await wrapper.setProps({ loading: true })
    await wrapper.get('button').trigger('click')
    expect(wrapper.emitted('refresh')).toHaveLength(1)
    wrapper.unmount()
  })
})
