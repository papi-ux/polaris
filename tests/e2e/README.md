# E2E tests

Playwright smoke tests for the Polaris web UI. These exercise the control plane (login, dashboard render, config views) only — streaming/capture paths are not testable from a browser.

## Mocked frontend lane

Run the fully mocked onboarding, manual-pairing, Live Tuning and update-channel
cases against the current checkout:

```
npm run test:e2e:mocked
```

This lane builds clean assets from this checkout and starts its own Vite preview
on `http://127.0.0.1:49386`. It needs no Polaris host or login credentials.
Live-host URL variables and CMake source/destination overrides do not change its
frontend. An existing server on that port causes a refusal rather than reuse.

The lane checks browser behavior with synthetic API evidence; it does not accept
host APIs, streaming, capture or a packaged host. Test assertions and retry policy
are the same as before the lanes were separated.

## Live-host lane

Polaris must already be running with a configured password.

```
POLARIS_URL=https://127.0.0.1:49001 \
POLARIS_USER=your-user \
POLARIS_PASS=your-password \
npm run test:e2e
```

These cases test the frontend assets served by that host, which can differ from
the checkout containing the specs. Record the installed host's exact package and
source before treating this as a source/host integration result. This lane
excludes the fully mocked cases above; a live-host pass does not run them.

First run also needs browsers installed:

```
npx playwright install chromium
```

## Fixtures

The onboarding layout suite runs against locally built assets with mocked APIs;
it does not need a running Polaris host or credentials. It checks all eight steps
at mobile, tablet, and desktop widths, and saves screenshots of the GPU and Encoder,
Launch Mode, Network and First App steps at each width.

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
