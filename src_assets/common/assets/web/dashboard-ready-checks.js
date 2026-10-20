export function buildReadyCheckDisplay(checks = []) {
  const normalized = checks.filter(Boolean).map(check => ({
    ...check,
    status: check.status || (check.ok === true ? 'ready' : check.ok === false ? 'attention' : 'not_checked'),
  }))
  const applicable = normalized.filter(check => check.status !== 'not_applicable')
  const passing = applicable.filter(check => check.status === 'ready').length
  const total = applicable.length
  const allPassing = total > 0 && passing === total
  const visibleChecks = allPassing ? [] : applicable.filter(check => check.status !== 'ready')
  const attention = visibleChecks.filter(check => check.status === 'attention')

  return {
    passing,
    total,
    allPassing,
    attention: attention.length,
    unknown: visibleChecks.filter(check => check.status === 'not_checked').length,
    notApplicable: normalized.length - total,
    primaryIssue: attention[0] || null,
    visibleChecks,
  }
}
