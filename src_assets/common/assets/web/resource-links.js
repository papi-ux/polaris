/**
 * The outbound links the web UI offers, defined once.
 *
 * ResourceCard.vue and HomeView.vue previously carried byte-identical copies of
 * both lists, so every edit had to be made twice or the two drifted.
 *
 * Where a link points is a judgement, not a preference. papi-ux.com renders the
 * repository's own docs/, so it is the better destination for the pages that
 * exist there. The wiki keeps the pages that live only in the wiki, and
 * Discussions stays on GitHub because that is where the discussion is.
 */

export const resources = [
  { href: 'https://papi-ux.com/docs/', labelKey: 'resource_card.documentation' },
  { href: 'https://papi-ux.com/docs/troubleshooting/', labelKey: 'resource_card.troubleshooting_guide' },
  { href: 'https://github.com/papi-ux/polaris/discussions', labelKey: 'resource_card.github_discussions' },
  { href: 'https://github.com/papi-ux/polaris/wiki', labelKey: 'resource_card.github_wiki' },
  { href: 'https://github.com/papi-ux/polaris/wiki/Stuttering-Clinic', labelKey: 'resource_card.github_stuttering_clinic' }
]

export const legalDocs = [
  { href: 'https://github.com/papi-ux/polaris/blob/master/LICENSE', labelKey: 'resource_card.license' },
  { href: 'https://github.com/papi-ux/polaris/blob/master/NOTICE', labelKey: 'resource_card.third_party_notice' }
]
