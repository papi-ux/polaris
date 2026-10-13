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

// mulberry32: a fixed sequence so the field's identity can be compared exactly.
function seeded(seed) {
  return () => {
    seed = (seed + 0x6D2B79F5) | 0
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed)
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

function viewport(width, height) {
  vi.stubGlobal('innerWidth', width)
  vi.stubGlobal('innerHeight', height)
}

const points = calls => calls.map(([x, y, radius]) => [x, y, radius])

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
    // A resize burst queues one frame, which draws one static frame and stops.
    const draws = contexts[0].clearRect.mock.calls.length
    viewport(800, 600)
    for (let i = 0; i < 3; i++) window.dispatchEvent(new Event('resize'))
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws)
    expect(queued.size).toBe(1)
    frame(16)
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
    // The 1280x800 reference surface draws each mode's full star count.
    vi.spyOn(HTMLElement.prototype, 'clientWidth', 'get').mockReturnValue(1280)
    vi.spyOn(HTMLElement.prototype, 'scrollHeight', 'get').mockReturnValue(800)
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

  it('keeps every star, nebula and galaxy across resizes and scales their positions', () => {
    Math.random.mockImplementation(seeded(404))
    motion.matches = true // static frames: no drift or twinkle between captures
    viewport(1280, 800)
    const wrapper = mounted()
    const stars = points(contexts[0].arc.mock.calls)
    const ambient = contexts[1].translate.mock.calls.map(call => [...call])
    const randomCalls = Math.random.mock.calls.length
    expect(stars.length).toBeGreaterThanOrEqual(280)

    contexts[0].arc.mockClear(); contexts[1].translate.mockClear()
    viewport(640, 400)
    window.dispatchEvent(new Event('resize'))
    expect(wrapper.element.style.width).toBe('640px') // the CSS box never lags layout
    frame(16)
    const halved = points(contexts[0].arc.mock.calls)
    // A quarter of the area draws the first quarter of the same stars, at half scale.
    expect(halved.length).toBeLessThan(stars.length)
    expect(halved).toEqual(stars.slice(0, halved.length).map(([x, y, radius]) => [x / 2, y / 2, radius]))
    expect(contexts[1].translate.mock.calls).toEqual(ambient.map(([x, y]) => [x / 2, y / 2]))

    contexts[0].arc.mockClear()
    viewport(1280, 800)
    window.dispatchEvent(new Event('resize'))
    frame(32)
    expect(points(contexts[0].arc.mock.calls)).toEqual(stars)
    expect(Math.random).toHaveBeenCalledTimes(randomCalls) // nothing was re-randomized
  })

  it('draws star density by surface area within each mode\'s bounds', () => {
    const cases = [
      [{}, 390, 844, 90],
      [{ dense: true }, 390, 844, 135],
      [{}, 1280, 800, 280],
      [{}, 3840, 2160, 280],
      [{}, 320, 200, 70],
    ]
    for (const [props, width, height, stars] of cases) {
      viewport(width, height)
      contexts = []
      const wrapper = mounted(props)
      expect(contexts[0].arc).toHaveBeenCalledTimes(stars)
      wrapper.unmount(); wrappers = []
    }
  })

  it('coalesces a resize burst into one backing store, backdrop and draw on the running loop', () => {
    let width = 1280
    vi.spyOn(HTMLElement.prototype, 'clientWidth', 'get').mockImplementation(() => width)
    vi.spyOn(HTMLElement.prototype, 'scrollHeight', 'get').mockReturnValue(800)
    const wrapper = mounted({ absolute: true })
    const draws = contexts[0].clearRect.mock.calls.length
    const transforms = contexts[0].setTransform.mock.calls.length
    const gradients = contexts[1].createRadialGradient.mock.calls.length
    width = 900
    for (let i = 0; i < 5; i++) {
      window.dispatchEvent(new Event('resize'))
      observer.callback()
    }
    expect(wrapper.element.style.width).toBe('900px')
    expect(wrapper.element.classList.contains('max-w-full')).toBe(true) // never wider than its pane
    expect(queued.size).toBe(1) // still only the loop
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws)
    frame(5) // inside the 30 Hz gate, but a resized surface draws now
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws + 1)
    expect(contexts[0].setTransform).toHaveBeenCalledTimes(transforms + 1)
    expect(contexts[1].createRadialGradient).toHaveBeenCalledTimes(gradients + 6 + 4) // balanced nebulae + galaxies
    expect(queued.size).toBe(1)
    frame(10)
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws + 1) // the gate holds again
  })

  it('defers hidden and particle-free resizes until the field draws again', () => {
    vi.stubGlobal('devicePixelRatio', 1)
    viewport(1280, 800)
    const wrapper = mounted()
    hidden = true; document.dispatchEvent(new Event('visibilitychange'))
    const draws = contexts[0].clearRect.mock.calls.length
    viewport(800, 600)
    window.dispatchEvent(new Event('resize'))
    expect(queued.size).toBe(0)
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws)
    expect(wrapper.element.width).toBe(1280)
    hidden = false; document.dispatchEvent(new Event('visibilitychange'))
    expect(wrapper.element.width).toBe(800)
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws + 1)
    expect(queued.size).toBe(1)

    setTheme('director')
    viewport(640, 400)
    window.dispatchEvent(new Event('resize'))
    expect(queued.size).toBe(0)
    expect(wrapper.element.width).toBe(800)
    setTheme('polaris')
    expect(wrapper.element.width).toBe(640)
    expect(queued.size).toBe(1)
  })

  it('draws once when the window and the pane observer report the same change', () => {
    motion.matches = true
    let width = 1280
    vi.spyOn(HTMLElement.prototype, 'clientWidth', 'get').mockImplementation(() => width)
    vi.spyOn(HTMLElement.prototype, 'scrollHeight', 'get').mockReturnValue(800)
    mounted({ absolute: true })
    const draws = contexts[0].clearRect.mock.calls.length
    width = 900
    window.dispatchEvent(new Event('resize'))
    frame(16)
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws + 1)
    observer.callback() // delivered after layout, one frame later, with the same size
    frame(32)
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws + 1)
    expect(queued.size).toBe(0)
  })

  it('drops a resize frame queued before the tab hid and applies the size on return', () => {
    motion.matches = true
    vi.stubGlobal('devicePixelRatio', 1)
    viewport(1280, 800)
    const wrapper = mounted()
    viewport(800, 600)
    window.dispatchEvent(new Event('resize'))
    expect(queued.size).toBe(1)
    hidden = true; document.dispatchEvent(new Event('visibilitychange'))
    const draws = contexts[0].clearRect.mock.calls.length
    const gradients = contexts[1].createRadialGradient.mock.calls.length
    while (queued.size) frame(16)
    // Nothing is reallocated or repainted while hidden.
    expect(wrapper.element.width).toBe(1280)
    expect(contexts[1].createRadialGradient).toHaveBeenCalledTimes(gradients)
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws)
    hidden = false; document.dispatchEvent(new Event('visibilitychange'))
    expect(wrapper.element.width).toBe(800)
    expect(contexts[0].clearRect).toHaveBeenCalledTimes(draws + 1)
    expect(queued.size).toBe(0)
  })

  it('cancels a queued resize frame on unmount', () => {
    motion.matches = true
    const wrapper = mounted()
    viewport(800, 600)
    window.dispatchEvent(new Event('resize'))
    expect(queued.size).toBe(1)
    wrapper.unmount(); wrappers = []
    expect(queued.size).toBe(0)
  })

  it('stays inert when a 2D context is unavailable', () => {
    HTMLCanvasElement.prototype.getContext.mockReturnValue(null)
    mounted()
    expect(queued.size).toBe(0)
  })
})
