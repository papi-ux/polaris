# Contributing

Thanks for taking the time to improve Polaris. Small, well-explained pull requests are easiest for me to review and merge.

## Expectations

- Keep pull requests focused and explain the user-facing impact.
- Test the exact paths your change affects before opening a PR.
- Do not commit local build trees, workspace files, maintainer notes, or private credentials.

## AI-Assisted Changes

AI-assisted drafting and refactoring are allowed. Please treat generated code like code you wrote yourself: understand it, review it, test it, and make sure the licensing is clear.

## Security-Sensitive Changes

Call out changes to authentication, pairing, trusted subnets, client commands, certificates, dependencies, release packaging, or privilege boundaries in the PR description. Those areas deserve a slower review pass.

## Before Opening a Pull Request

Run the checks that match your change from a clean checkout or isolated worktree. Documentation, dependency, and release-package changes should include:

```bash
bash scripts/check-public-docs.sh
python3 scripts/check-release-package-dependencies.py
npm ci
npm audit --audit-level=high
```

For C++ changes, configure and build the affected production targets and run their matching test binaries. Release-package changes also need the relevant package build and installed-layout smoke test; ordinary branch CI does not replace exact-tag verification.

Do not rewrite or force-push another contributor's commits to make a review pass. Follow-up fixes should be additive commits so the review history remains inspectable.
