import { defineConfig } from 'vitest/config'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  // Absolute URLs name files in the web root's public directory, which the browser loads as they
  // are. Keep them as strings so components that use one, like App.vue's wordmark, can be mounted.
  plugins: [vue({ template: { transformAssetUrls: { includeAbsolute: false } } })],
  resolve: {
    alias: {
      vue: 'vue/dist/vue.esm-bundler.js',
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    include: ['src_assets/common/assets/web/**/*.test.js'],
  },
})
