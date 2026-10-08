# E2E tests

Playwright smoke tests for the Polaris web UI. These exercise the control plane (login, dashboard render, config views) only — streaming/capture paths are not testable from a browser.

## Mocked frontend lane

Run the mocked onboarding, manual-pairing, Live Tuning, update-channel and
Polaris settings UX cases against the current checkout:

```
npm run test:e2e:mocked
```

This lane builds clean assets from this checkout and starts its own Vite preview
on `http://127.0.0.1:49386`. It needs no Polaris host or login credentials.
Live-host URL variables and CMake source/destination overrides do not change its
frontend. An existing server on that port causes a refusal rather than reuse.

The preview carries a test-only identity for the checkout revision and frontend
source bytes. Both startup and page navigation check that identity. The result
directory records it in `frontend-identity.json`. Generated HTML and caches do
not affect the source fingerprint; new unignored frontend inputs do.

Host API calls must be fulfilled by a case's mocks. A shared fixture supplies
deterministic GitHub release responses when a case does not override them, and
fails unexpected external or unmocked API requests. Service workers are blocked
so they cannot bypass request interception. The mocked lane uses zero retries,
including in CI; an initial failure is a failure. It records a trace for every
case, so first failures also keep their browser history when a guard fails during
context cleanup. Trace recording ends before the browser closes; the final guard
verdict still includes violations observed during closure.

This checks browser behavior with synthetic API evidence. Host APIs, streaming,
capture and a packaged host need separate acceptance. Existing assertions and
the live/mock case partition are preserved. List the current case inventories
without contacting a host:

```
npm run test:e2e:mocked -- --list
npm run test:e2e -- --list
```

## Live-host lane

Polaris must already be running with a configured password. The target URL must
be explicit; the lane never selects an implicit localhost host. Authenticated
cases fail if credentials are missing rather than silently skipping.

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
Its startup output identifies the selected origin and labels the frontend as
host-served. That origin is not proof that the host serves this checkout. The
existing explicit login compatibility skips remain visible in the result.

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
npx playwright test --config playwright.welcome.config.js
```

The welcome and manual-pairing configs use the same clean build, source-identity
and request guards as the mocked lane, on ports 49388 and 49387 respectively.
They retain their original filtered case selections and zero-retry behavior:

```
npx playwright test --config playwright.pairing.config.js
```

`fixtures/auth.js` provides a `loggedInPage` fixture that clears cookies and signs
in for each test. Live tests that need to sign in import from there. Mocked tests
import `test` and `expect` from `fixtures/mocked.js` to keep source and request
guards active.

## Scope

- Login form rendering + invalid-credential error
- Dashboard loads and the six metric chart cards render
- Config view loads and tabs are reachable
- Apps view loads

Anything requiring a Moonlight client is out of scope.
