import { mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

import ConfirmActionDialog from './ConfirmActionDialog.vue'

const baseProps = {
  modelValue: true,
  title: 'Restart Polaris?',
  message: 'This will interrupt connected streams while the host service restarts.',
  impactItems: ['Active streams disconnect', 'Browser access may briefly drop'],
  confirmLabel: 'Restart',
  cancelLabel: 'Keep running',
}

function mountDialog(props = {}) {
  return mount(ConfirmActionDialog, {
    attachTo: document.body,
    props: { ...baseProps, ...props },
  })
}

function bodyGet(selector) {
  const element = document.body.querySelector(selector)
  expect(element).toBeTruthy()
  return element
}

describe('ConfirmActionDialog', () => {
  afterEach(() => {
    document.body.innerHTML = ''
    vi.restoreAllMocks()
  })

  it('summarizes impact and restores focus when cancelled', async () => {
    const opener = document.createElement('button')
    opener.textContent = 'Open confirm'
    document.body.appendChild(opener)
    opener.focus()

    const wrapper = mountDialog()
    await wrapper.vm.$nextTick()

    expect(bodyGet('[role="dialog"]').getAttribute('aria-modal')).toBe('true')
    expect(document.body.textContent).toContain('Active streams disconnect')
    expect(document.activeElement).toBe(bodyGet('[data-confirm-cancel]'))

    bodyGet('[data-confirm-cancel]').click()
    await wrapper.vm.$nextTick()

    expect(wrapper.emitted('cancel')).toHaveLength(1)
    expect(wrapper.emitted('update:modelValue')[0]).toEqual([false])
    expect(document.activeElement).toBe(opener)
  })

  it('emits confirm without closing so async actions can show progress', async () => {
    const wrapper = mountDialog({ pending: true, pendingLabel: 'Restarting…' })
    await wrapper.vm.$nextTick()

    expect(bodyGet('[data-confirm-confirm]').hasAttribute('disabled')).toBe(true)
    expect(document.body.textContent).toContain('Restarting…')

    await wrapper.setProps({ pending: false })
    bodyGet('[data-confirm-confirm]').click()
    await wrapper.vm.$nextTick()

    expect(wrapper.emitted('confirm')).toHaveLength(1)
    expect(wrapper.emitted('update:modelValue')).toBeUndefined()
  })

  it('supports Escape cancellation unless an async action is pending', async () => {
    const wrapper = mountDialog()
    await wrapper.vm.$nextTick()

    bodyGet('[role="dialog"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    await wrapper.vm.$nextTick()
    expect(wrapper.emitted('cancel')).toHaveLength(1)

    await wrapper.setProps({ pending: true })
    bodyGet('[role="dialog"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    await wrapper.vm.$nextTick()
    expect(wrapper.emitted('cancel')).toHaveLength(1)
  })

  it('traps Tab focus inside the dialog controls', async () => {
    const wrapper = mountDialog()
    await wrapper.vm.$nextTick()

    const cancel = bodyGet('[data-confirm-cancel]')
    const confirm = bodyGet('[data-confirm-confirm]')

    cancel.focus()
    bodyGet('[role="dialog"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true, shiftKey: true }))
    await wrapper.vm.$nextTick()
    expect(document.activeElement).toBe(confirm)

    confirm.focus()
    bodyGet('[role="dialog"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true }))
    await wrapper.vm.$nextTick()
    expect(document.activeElement).toBe(cancel)
  })
})
