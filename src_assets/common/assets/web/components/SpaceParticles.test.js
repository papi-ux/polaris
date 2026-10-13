import { mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import SpaceParticles from './SpaceParticles.vue'
import { setTheme } from '../theme.js'

vi.mock('../theme-bridge.js', () => ({
  readThemeTokens: () => ({ ice: '#c8d6e5', accent: '#7c73ff', 'accent-2': '#7c73ff' }),
}))

function drawingContext() {
  return Object.fromEntries([
    'setTransform', 'clearRect', 'save', 'restore', 'translate', 'rotate', 'scale',
    'fillRect', 'beginPath', 'arc', 'fill', 'stroke', 'moveTo', 'lineTo', 'drawImage',
  ].map(name => [name, vi.fn()]).concat([
    ['createRadialGradient', vi.fn(() => ({ addColorStop: vi.fn() }))],
    ['createLinearGradient', vi.fn(() => ({ addColorStop: vi.fn() }))],
  ]))
}

let wrappers, contexts, queued, motion, hidden, observer

function mounted(props = {}) {
  const wrapper = mount(SpaceParticles, { props, attachTo: document.body })
  wrappers.push(wrapper)
  return wrapper
}

function frame(time) {
  const [id, callback] = queued.entries().next().value
  queued.delete(id)
  callback(time)
}

describe('space field lifecycle and draw bounds', () => {
  beforeEach(() => {
    localStorage.clear()
    document.documentElement.removeAttribute('data-theme')
    wrappers = []; contexts = []; queued = new Map(); hidden = false
    let nextId = 0
    vi.stubGlobal('requestAnimationFrame', vi.fn(callback => {
      const id = nextId++; queued.set(id, callback); return id
    }))
    vi.stubGlobal('cancelAnimationFrame', vi.fn(id => queued.delete(id)))
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(() => {
      const ctx = drawingContext(); contexts.push(ctx); return ctx
    })
    motion = new EventTarget(); motion.matches = false
    vi.stubGlobal('matchMedia', vi.fn(() => motion))
    vi.spyOn(document, 'hidden', 'get').mockImplementation(() => hidden)
    vi.spyOn(Math, 'random').mockReturnValue(0.5) // no shooting stars or bright blooms
    vi.stubGlobal('ResizeObserver', class {
      constructor(callback) { observer = { callback, observe: vi.fn(), disconnect: vi.fn() }; return observer }
    })
  })

  afterEach(() => {
    for (const wrapper of wrappers) wrapper.unmount()
    document.body.innerHTML = ''
    vi.restoreAllMocks()
    vi.unstubAllGlobals()
  })

  it('is decorative and input-transparent, caps DPR, and reuses cached ambient gradients', () => {
    vi.stubGlobal('devicePixelRatio', 8)
    const wrapper = mounted()
    const canvas = wrapper.element
    expect(canvas.getAttribute('aria-hidden')).toBe('true')
    expect(canvas.classList.contains('pointer-events-none')).toBe(true)
    expect(canvas.width).toBeLessThanOrEqual(window.innerWidth * 2)
    expect(canvas.width * canvas.height).toBeLessThanOrEqual(4_000_000)
    expect(contexts).toHaveLength(2)
    expect(contexts[1].createRadialGradient).toHaveBeenCalled()
    const cachedCount = contexts[1].createRadialGradient.mock.calls.length
    const draws = contexts[0].clearRect.mock.calls.length
    frame(10) // below the 30 Hz drawing interval
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws)
    frame(40)
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws + 1)
    expect(contexts[0].drawImage).toHaveBeenCalled()
    expect(contexts[1].createRadialGradient).toHaveBeenCalledTimes(cachedCount)
    expect(contexts[0].createRadialGradient).not.toHaveBeenCalled()
  })

  it('bounds both backing stores for a long absolute scroll surface', () => {
    vi.stubGlobal('devicePixelRatio', 4)
    vi.spyOn(HTMLElement.prototype, 'clientWidth', 'get').mockReturnValue(3840)
    vi.spyOn(HTMLElement.prototype, 'scrollHeight', 'get').mockReturnValue(20000)
    const wrapper = mounted({ absolute: true, dense: true })
    expect(wrapper.element.height).toBeLessThanOrEqual(8192)
    expect(wrapper.element.width * wrapper.element.height).toBeLessThanOrEqual(4_000_000)
    const image = contexts[0].drawImage.mock.calls[0][0]
    expect(image.width).toBe(wrapper.element.width)
    expect(image.height).toBe(wrapper.element.height)
    observer.callback()
    expect(queued.size).toBe(1)
  })

  it('draws a static reduced-motion frame and reacts to preference changes without RAF duplication', () => {
    motion.matches = true
    mounted()
    expect(contexts[0].clearRect).toHaveBeenCalled()
    expect(queued.size).toBe(0)
    motion.matches = false; motion.dispatchEvent(new Event('change'))
    expect(queued.size).toBe(1)
    motion.matches = true; motion.dispatchEvent(new Event('change'))
    expect(queued.size).toBe(0)
    const draws = contexts[0].clearRect.mock.calls.length
    window.dispatchEvent(new Event('resize'))
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws + 1)
    expect(queued.size).toBe(0)
  })

  it('suspends hidden tabs, including theme changes while hidden, and resumes one loop', () => {
    mounted()
    hidden = true; document.dispatchEvent(new Event('visibilitychange'))
    expect(queued.size).toBe(0)
    const draws = contexts[0].clearRect.mock.calls.length
    setTheme('miami')
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws)
    hidden = false; document.dispatchEvent(new Event('visibilitychange'))
    expect(queued.size).toBe(1)
    frame(60000)
    expect(queued.size).toBe(1)
  })

  it('keeps OLED black and Director opaque with no animation, then rebuilds an enabled theme', () => {
    const wrapper = mounted()
    for (const theme of ['oled', 'director']) {
      setTheme(theme)
      expect(wrapper.element.style.display).toBe('none')
      expect(wrapper.element.style.opacity).toBe('0')
      expect(queued.size).toBe(0)
    }
    setTheme('high-contrast')
    expect(wrapper.element.style.display).toBe('')
    expect(wrapper.element.style.opacity).toBe('0.28')
    expect(queued.size).toBe(1)
  })

  it('rebuilds mode counts without adding another loop and releases listeners/observer on unmount', async () => {
    const wrapper = mounted({ absolute: true })
    contexts[0].arc.mockClear()
    await wrapper.setProps({ dense: true })
    expect(contexts[0].arc).toHaveBeenCalledTimes(420)
    expect(queued.size).toBe(1)
    wrapper.unmount(); wrappers = []
    expect(queued.size).toBe(0)
    expect(observer.disconnect).toHaveBeenCalledOnce()
    const draws = contexts[0].clearRect.mock.calls.length
    window.dispatchEvent(new Event('resize'))
    document.dispatchEvent(new Event('visibilitychange'))
    motion.dispatchEvent(new Event('change'))
    setTheme('miami')
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws)
  })

  it('stays inert when a 2D context is unavailable', () => {
    HTMLCanvasElement.prototype.getContext.mockReturnValue(null)
    mounted()
    expect(queued.size).toBe(0)
  })
})
