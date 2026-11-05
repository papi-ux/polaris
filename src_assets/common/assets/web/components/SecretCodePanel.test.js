import { readFileSync } from 'node:fs'
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import SecretCodePanel from './SecretCodePanel.vue'
import { isStarTrailCode, VERSION_PRESS_GAP_MS, VERSION_PRESSES, versionPressCounter } from '../secret-code.js'

const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const reducedMotion = matches => vi.stubGlobal('matchMedia', () => ({ matches }))
let wrapper, opener
async function open() {
  opener = document.createElement('button')
  document.body.append(opener)
  opener.focus()
  const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
  wrapper = mount(SecretCodePanel, { attachTo: document.body, global: { plugins: [i18n] } })
  await flushPromises()
}
const panel = () => document.querySelector('[data-secret-code-panel]')
const input = () => document.querySelector('[data-secret-code-input]')
async function enter(text) {
  input().value = text
  input().dispatchEvent(new Event('input'))
  panel().querySelector('form').dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
  await flushPromises()
}
const press = async key => {
  document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true }))
  await flushPromises()
}

beforeEach(() => reducedMotion(false))
afterEach(() => {
  wrapper?.unmount()
  wrapper = null
  opener?.remove()
  document.body.innerHTML = ''
  vi.unstubAllGlobals()
})

describe('version press counter', () => {
  it('opens on the fifth press in a row, then starts over', () => {
    let time = 1000
    const pressed = versionPressCounter({ now: () => time })
    const results = []
    for (let i = 0; i < VERSION_PRESSES * 2; i++) { results.push(pressed()); time += 200 }
    expect(results).toEqual([false, false, false, false, true, false, false, false, false, true])
  })

  it('starts over after a long pause, so scattered presses never add up', () => {
    let time = 1000
    const pressed = versionPressCounter({ now: () => time })
    for (let i = 0; i < VERSION_PRESSES - 1; i++) { expect(pressed()).toBe(false); time += 200 }
    time += VERSION_PRESS_GAP_MS + 1
    for (let i = 0; i < VERSION_PRESSES - 1; i++) { expect(pressed()).toBe(false); time += 200 }
    expect(pressed()).toBe(true)
  })
})

describe('isStarTrailCode', () => {
  it('accepts POLARIS spelled any way and nothing else', () => {
    for (const code of ['POLARIS', 'polaris', ' Polaris ']) expect(isStarTrailCode(code)).toBe(true)
    for (const code of ['', 'POLARI', 'POLARIS!', 'NOVA', null, undefined]) expect(isStarTrailCode(code)).toBe(false)
  })
})

describe('SecretCodePanel', () => {
  it('is a labelled dialog with a labelled field, and takes keyboard focus', async () => {
    await open()
    const label = panel().querySelector('label')
    expect(label.textContent.trim()).toBe('Enter a code')
    expect(label.getAttribute('for')).toBe(input().id)
    expect(panel().getAttribute('role')).toBe('dialog')
    expect(panel().getAttribute('aria-modal')).toBe('true')
    expect(panel().getAttribute('aria-labelledby')).toBe(label.id)
    expect(document.activeElement).toBe(input())
  })

  it('keeps Tab inside the panel and closes on Escape, back on the version', async () => {
    await open()
    const close = panel().querySelector('[data-secret-code-close]')
    const submit = panel().querySelector('[data-secret-code-submit]')
    submit.focus()
    await press('Tab')
    expect(document.activeElement).toBe(input())
    document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, bubbles: true, cancelable: true }))
    expect(document.activeElement).toBe(submit)
    close.focus()
    await press('Escape')
    expect(wrapper.emitted('close')).toHaveLength(1)
    wrapper.unmount(); wrapper = null
    expect(document.activeElement).toBe(opener)
  })

  it('plays the star trails for POLARIS, and tells a screen reader', async () => {
    await open()
    await enter('polaris')
    const trail = panel().querySelector('[data-star-trail]')
    expect(trail.getAttribute('data-star-trail-motion')).toBe('animated')
    expect(trail.getAttribute('role')).toBe('img')
    expect(trail.getAttribute('aria-label')).toBe('Star trails circling the Polaris logo')
    expect(trail.querySelector('img').getAttribute('src')).toBe('/images/logo-polaris.svg')
    expect(panel().querySelector('[role=status]').textContent).toBe('Star trails circling the Polaris logo')
  })

  it('gives a still image under reduced motion', async () => {
    reducedMotion(true)
    await open()
    await enter('POLARIS')
    const trail = panel().querySelector('[data-star-trail]')
    expect(trail.classList.contains('is-still')).toBe(true)
    expect(trail.getAttribute('data-star-trail-motion')).toBe('still')
  })

  it('ignores an unknown code quietly: no trail, no message, and the field is ready again', async () => {
    await open()
    const before = panel().textContent
    await enter('UP UP DOWN DOWN')
    expect(panel().querySelector('[data-star-trail]')).toBeNull()
    expect(panel().querySelector('[role=status]').textContent).toBe('')
    expect(panel().textContent).toBe(before)
    expect(input().value).toBe('')
    expect(wrapper.emitted('close')).toBeUndefined()
  })

  it('runs its animation only without a reduced-motion preference, once, and moves nothing else', () => {
    const source = readFileSync('src_assets/common/assets/web/components/SecretCodePanel.vue', 'utf8')
    const style = source.slice(source.indexOf('<style scoped>'))
    const gate = style.slice(style.indexOf('@media (prefers-reduced-motion: no-preference)'), style.indexOf('@keyframes'))
    expect(gate).toContain('animation: star-trail-turn')
    expect(style.match(/animation:/g)).toHaveLength(1)
    expect(style).not.toContain('infinite')
    // Lazy: nothing of the panel sits in the startup bundle or stylesheet.
    const app = readFileSync('src_assets/common/assets/web/App.vue', 'utf8')
    expect(app).toContain("defineAsyncComponent(() => import('./components/SecretCodePanel.vue'))")
    expect(readFileSync('src_assets/common/assets/web/app.css', 'utf8')).not.toContain('star-trail')
  })
})

describe('the version in the sidebar', () => {
  const app = readFileSync('src_assets/common/assets/web/App.vue', 'utf8')

  it('is a real button, so Enter, a click and a screen reader all reach it', () => {
    const start = app.indexOf('data-app-version')
    const element = app.slice(app.lastIndexOf('<', start), app.indexOf('</button>', start))
    expect(element.startsWith('<button')).toBe(true)
    expect(element).toContain('type="button"')
    expect(element).toContain('@click="pressVersion"')
    expect(element).toContain(':aria-label=')
    expect(app).toContain('<SecretCodePanel v-if="codePanelOpen" @close="codePanelOpen = false" />')
  })
})
