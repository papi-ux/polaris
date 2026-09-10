# E2E tests

Playwright smoke tests for the Polaris web UI. These exercise the control plane (login, dashboard render, config views) only — streaming/capture paths are not testable from a browser.

## Running

Polaris must already be running with a configured password.

```
POLARIS_URL=https://127.0.0.1:49001 \
POLARIS_USER=your-user \
POLARIS_PASS=your-password \
npm run test:e2e
```

First run also needs browsers installed:

```
npx playwright install chromium
```

## Fixtures

The onboarding layout suite runs against locally built assets with mocked APIs;
it does not need a running Polaris host or credentials. It checks all five steps
at mobile, tablet, and desktop widths, and saves a First App screenshot per width.

```
npm run build
npx playwright test --config playwright.welcome.config.js
```

`fixtures/auth.js` provides a `loggedInPage` fixture that POSTs `/api/login` once per worker and reuses the session cookie. Tests that need to be signed in should import from there.

## Scope

- Login form rendering + invalid-credential error
- Dashboard loads and the six metric chart cards render
- Config view loads and tabs are reachable
- Apps view loads

Anything requiring a Moonlight client is out of scope.
