// Applies the stored theme before first paint so a non-default theme never
// flashes the default palette. Loaded as a classic blocking script from
// template_header.html because the production CSP is script-src 'self'
// (no inline scripts), and served from /assets/ because that is a route the
// Polaris host actually exposes (the web root is not served).
//
// This file intentionally mirrors theme.js: the storage key, the id list,
// and the default-means-no-attribute rule. theme.test.js pins the two files
// together, so change them in lockstep.
(function () {
  var KNOWN_THEMES = ['polaris', 'portable-chrome', 'oled', 'miami', 'director', 'high-contrast']
  // Inside Nova's Host Console the theme Nova named shows for the visit: from the first load's
  // query, then from the visit's session storage (client-embed.js keeps both in step).
  var visit = null
  try {
    var params = new URLSearchParams(location.search)
    if (params.get('client') === 'nova') {
      visit = params.get('theme')
    } else {
      var kept = JSON.parse(sessionStorage.getItem('polaris-embed') || 'null')
      visit = kept && kept.client === 'nova' ? kept.theme : null
    }
  } catch (e) {
    visit = null
  }
  try {
    var theme = visit && KNOWN_THEMES.indexOf(visit) !== -1 ? visit : localStorage.getItem('polaris-theme')
    if (theme && theme !== 'polaris' && KNOWN_THEMES.indexOf(theme) !== -1) {
      document.documentElement.setAttribute('data-theme', theme)
    }
  } catch (e) {
    // Storage unavailable: stay on the default theme.
  }
})()
