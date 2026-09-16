// The explanation providers Polaris can use, shared by the AI settings tab and
// the first-run wizard so there is one list of names, defaults and profiles.
// Keys ending in Key are locale keys resolved by the component that renders them.

export const providerOptions = [
  {
    id: 'anthropic',
    name: 'Claude',
    eyebrowKey: 'config.ai_provider_anthropic_eyebrow',
    summaryKey: 'config.ai_provider_anthropic_summary',
    defaultModel: 'claude-haiku-4-5-20251001',
    defaultBaseUrl: 'https://api.anthropic.com',
    defaultAuth: 'subscription',
    authModes: ['subscription', 'api_key'],
    accent: 'border-warning/30 bg-warning/8 text-warning-bright',
    pill: 'text-warning-bright border-warning/30',
    subscriptionLabel: 'Claude CLI',
    subscriptionBinary: 'claude',
    subscriptionLoginCommand: 'claude auth login',
    keyPlaceholder: 'sk-ant-api03-...',
    keyHintKey: 'config.ai_provider_anthropic_key_hint',
    profiles: [
      {
        id: 'claude-cli',
        name: 'Claude CLI',
        descriptionKey: 'config.ai_profile_claude_cli_desc',
        model: 'claude-haiku-4-5-20251001',
        baseUrl: 'https://api.anthropic.com',
        authMode: 'subscription'
      },
      {
        id: 'anthropic-api',
        name: 'Anthropic API',
        descriptionKey: 'config.ai_profile_anthropic_api_desc',
        model: 'claude-haiku-4-5-20251001',
        baseUrl: 'https://api.anthropic.com',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'openai',
    name: 'OpenAI',
    eyebrowKey: 'config.ai_provider_openai_eyebrow',
    summaryKey: 'config.ai_provider_openai_summary',
    defaultModel: 'gpt-5.4-mini',
    defaultBaseUrl: 'https://api.openai.com/v1',
    defaultAuth: 'subscription',
    authModes: ['subscription', 'api_key'],
    accent: 'border-success/30 bg-success/8 text-success-bright',
    pill: 'text-success-bright border-success/30',
    subscriptionLabel: 'Codex CLI',
    subscriptionBinary: 'codex',
    subscriptionLoginCommand: 'codex login',
    keyPlaceholder: 'sk-proj-...',
    keyHintKey: 'config.ai_provider_openai_key_hint',
    profiles: [
      {
        id: 'codex-cli',
        name: 'Codex CLI',
        descriptionKey: 'config.ai_profile_codex_cli_desc',
        model: 'gpt-5.4-mini',
        baseUrl: 'https://api.openai.com/v1',
        authMode: 'subscription'
      },
      {
        id: 'openai-default',
        name: 'Hosted API',
        descriptionKey: 'config.ai_profile_openai_default_desc',
        model: 'gpt-5.4-mini',
        baseUrl: 'https://api.openai.com/v1',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'gemini',
    name: 'Gemini',
    eyebrowKey: 'config.ai_provider_gemini_eyebrow',
    summaryKey: 'config.ai_provider_gemini_summary',
    defaultModel: 'gemini-2.5-flash',
    defaultBaseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
    defaultAuth: 'api_key',
    authModes: ['api_key'],
    accent: 'border-info/30 bg-info/8 text-info-bright',
    pill: 'text-info-bright border-info/30',
    keyPlaceholder: 'AIza...',
    keyHintKey: 'config.ai_provider_gemini_key_hint',
    profiles: [
      {
        id: 'gemini-default',
        name: 'Gemini API',
        descriptionKey: 'config.ai_profile_gemini_default_desc',
        model: 'gemini-2.5-flash',
        baseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'deepseek',
    name: 'DeepSeek',
    eyebrowKey: 'config.ai_provider_deepseek_eyebrow',
    summaryKey: 'config.ai_provider_deepseek_summary',
    defaultModel: 'deepseek-v4-flash',
    defaultBaseUrl: 'https://api.deepseek.com',
    defaultAuth: 'api_key',
    defaultTimeout: 30000,
    authModes: ['api_key'],
    accent: 'border-info/30 bg-info/8 text-info-bright',
    pill: 'text-info-bright border-info/30',
    keyPlaceholder: 'sk-...',
    keyHintKey: 'config.ai_provider_deepseek_key_hint',
    profiles: [
      {
        id: 'deepseek-default',
        name: 'DeepSeek API',
        descriptionKey: 'config.ai_profile_deepseek_default_desc',
        model: 'deepseek-v4-flash',
        baseUrl: 'https://api.deepseek.com',
        authMode: 'api_key'
      }
    ]
  },
  {
    id: 'local',
    name: 'Local',
    eyebrowKey: 'config.ai_provider_local_eyebrow',
    summaryKey: 'config.ai_provider_local_summary',
    defaultModel: 'gpt-oss',
    defaultBaseUrl: 'http://127.0.0.1:11434/v1',
    defaultAuth: 'none',
    authModes: ['none', 'api_key'],
    accent: 'border-storm/25 bg-storm/8 text-silver',
    pill: 'text-silver border-storm/25',
    keyPlaceholder: 'optional',
    keyHintKey: 'config.ai_provider_local_key_hint',
    profiles: [
      {
        id: 'ollama',
        name: 'Ollama',
        descriptionKey: 'config.ai_profile_ollama_desc',
        model: 'gpt-oss',
        baseUrl: 'http://127.0.0.1:11434/v1',
        authMode: 'none'
      },
      {
        id: 'lm-studio',
        name: 'LM Studio',
        descriptionKey: 'config.ai_profile_lm_studio_desc',
        model: 'qwen3-8b',
        baseUrl: 'http://127.0.0.1:1234/v1',
        authMode: 'none'
      }
    ]
  }
]

export const authModeLabels = {
  subscription: { nameKey: 'config.ai_auth_subscription', descriptionKey: 'config.ai_auth_subscription_desc' },
  api_key: { nameKey: 'config.ai_auth_api_key', descriptionKey: 'config.ai_auth_api_key_desc' },
  none: { nameKey: 'config.ai_auth_none', descriptionKey: 'config.ai_auth_none_desc' }
}

export function providerDefaultTimeout(provider, authMode = provider?.defaultAuth) {
  if (provider?.id === 'anthropic' && authMode === 'subscription') return 30000
  return provider?.defaultTimeout || (provider?.id === 'local' ? 60000 : 5000)
}
