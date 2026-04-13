import js from '@eslint/js'
import globals from 'globals'
import vue from 'eslint-plugin-vue'
import vueParser from 'vue-eslint-parser'

const baseJsRules = {
  'no-console': 'off',
  'no-unused-vars': ['error', {
    argsIgnorePattern: '^_',
    caughtErrors: 'none',
    ignoreRestSiblings: true,
  }],
}

export default [
  {
    ignores: [
      'build/**',
      'build-local/**',
      'node_modules/**',
      'packaging/**',
      'src_assets/common/assets/web/public/**',
    ],
  },
  {
    ...js.configs.recommended,
    files: ['src_assets/common/assets/web/**/*.js'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      globals: {
        ...globals.browser,
      },
    },
    rules: baseJsRules,
  },
  {
    files: ['src_assets/common/assets/web/**/*.vue'],
    plugins: {
      vue,
    },
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      parser: vueParser,
      parserOptions: {
        ecmaVersion: 'latest',
        sourceType: 'module',
      },
      globals: {
        ...globals.browser,
      },
    },
    rules: {
      ...baseJsRules,
      'vue/no-parsing-error': 'error',
    },
  },
]
