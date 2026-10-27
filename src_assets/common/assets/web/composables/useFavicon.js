import { inject, onScopeDispose, watch, watchEffect } from 'vue'
import { useStreamStats } from './useStreamStats.js'

export function useFavicon(stats) {
  // Observe availability on the existing shared channel, with no second transport.
  const { connected } = useStreamStats()
  const i18n = inject('i18n', null)
  const originalTitle = document.title
  watchEffect(() => {
    const live = connected.value && stats.value?.streaming === true
    const name = typeof stats.value?.client_name === 'string' ? stats.value.client_name.trim() : ''
    const label = name
      ? (i18n?.t('browser_tab.streaming_to', { device: name }) ?? `Streaming to ${name}`)
      : (i18n?.t('browser_tab.streaming') ?? 'Streaming')
    document.title = live ? `● ${label}` : originalTitle
  })
  onScopeDispose(() => { document.title = originalTitle })

  watch(() => stats.value?.streaming, (streaming) => {
    const links = document.querySelectorAll("link[rel='icon']")
    if (!links.length) {
      return
    }

    const ico = '/images/polaris-favicon.ico?v=20260726'
    const svg = '/images/polaris-favicon.svg?v=20260726'

    links.forEach((link) => {
      const isSvg = link.getAttribute('type') === 'image/svg+xml'
      link.href = isSvg ? svg : (streaming ? svg : ico)
    })
  })
}
