import { watch } from 'vue'

export function useFavicon(stats) {
  watch(() => stats.value?.streaming, (streaming) => {
    const link = document.querySelector("link[rel='icon']")
    if (link) {
      link.href = streaming ? '/images/polaris-playing.svg' : '/images/polaris.ico'
    }
  })
}
