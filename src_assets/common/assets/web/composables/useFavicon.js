import { watch } from 'vue'

export function useFavicon(stats) {
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
