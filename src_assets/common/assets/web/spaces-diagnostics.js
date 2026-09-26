import { validSetup } from './spaces-setup.js'
import { validSnapshot } from './spaces-access.js'

/** Read-only findings from the same validated snapshots the Spaces page uses. */
export function spacesDiagnostics({ setup, snapshot, setupError = '', snapshotError = '' }, t) {
  const host = validSetup(setup) ? setup : null
  const state = validSnapshot(snapshot) ? snapshot : null
  const disabled = host?.configured === false && state?.enabled === false && !state.failed && !state.changing
  const updating = state?.changing || ['downloading', 'moving', 'creating'].includes(state?.runtime_move_job?.state)
  const groups = []
  const text = (key, params) => t(`spaces_doctor.${key}`, params)
  const row = (id, status, title, detail) => ({ id, status, title, detail })
  const hostRows = []
  const runtimeRows = []
  const activityRows = []
  if (!host || setupError) {
    hostRows.push(row('setup-unavailable', 'warning', text('host'), text('setup_unavailable')))
  } else {
    for (const check of host.checks) {
      const status = check.state === 'ready' ? 'pass' : disabled || check.state === 'not_configured' ? 'info' : 'warning'
      const finding = row(check.id, status, check.title, check.detail)
      const destination = ['runtime', 'nvidia_libraries'].includes(check.id) ? runtimeRows : hostRows
      destination.push(finding)
    }
    if (!host.checks.some(check => check.id === 'runtime')) {
      runtimeRows.push(row('runtime-unreported', 'info', text('runtime'), text('runtime_unreported')))
    }
  }
  if (!state || snapshotError) {
    activityRows.push(row('state-unavailable', 'warning', text('controller'), text('state_unavailable')))
  } else {
    const controllerStatus = state.failed ? 'fail' : state.changing || !state.enabled ? 'info' : state.available ? 'pass' : 'warning'
    const controllerDetail = state.failed ? 'controller_failed' : state.changing ? 'controller_changing' :
      !state.enabled ? 'disabled_detail' : state.available ? 'controller_ready' : 'controller_unavailable'
    activityRows.push(row('controller', controllerStatus, text('controller'), text(controllerDetail)))
    if (host && !setupError && (host.configured !== state.enabled || host.available !== state.available)) {
      activityRows.push(row('snapshot-changed', 'warning', text('changed_title'), text('changed_detail')))
    }
    const profiles = state.profiles.filter(profile => !profile.archived)
    for (const profile of profiles.filter(profile => profile.runtime_mismatch).slice(0, 50)) {
      runtimeRows.push(row(`driver-${profile.id}`, 'warning', profile.name, text('driver_mismatch', {
        runtime: profile.runtime_driver, host: profile.host_driver,
      })))
    }
    const unsupported = profiles.filter(profile => profile.family === '').length
    if (unsupported) runtimeRows.push(row('unsupported-launchers', 'warning', text('launchers'), text('unsupported_launchers', { count: unsupported })))
    const mismatches = profiles.filter(profile => profile.runtime_mismatch).length
    if (mismatches > 50) runtimeRows.push(row('more-mismatches', 'warning', text('runtime'), text('more_mismatches', { count: mismatches - 50 })))
    const unreported = profiles.filter(profile => profile.runtime_mismatch === undefined).length
    if (unreported) runtimeRows.push(row('compatibility-unreported', 'info', text('compatibility'), text('compatibility_unreported', { count: unreported })))
    if (state.runtime_move_job?.state === 'failed') {
      runtimeRows.push(row('runtime-job-failed', 'warning', text('runtime_job'), text('runtime_job_failed')))
    } else if (state.runtime_move_job && ['downloading', 'moving', 'creating'].includes(state.runtime_move_job.state)) {
      runtimeRows.push(row('runtime-job-pending', 'info', text('runtime_job'), text('runtime_job_pending')))
    }
    if (!Array.isArray(state.activity)) {
      activityRows.push(row('activity-unreported', 'info', text('activity'), text('activity_unreported')))
    } else {
      const counts = { running: 0, starting: 0, stopping: 0 }
      state.activity.forEach(item => { counts[item.state] += 1 })
      activityRows.push(row('activity', 'info', text('activity'), text('activity_counts', counts)))
    }
    if (state.capacity && state.capacity.concurrent_limit > 0) {
      activityRows.push(row('capacity', 'info', text('capacity'), text('capacity_counts', {
        active: state.capacity.concurrent_active, limit: state.capacity.concurrent_limit,
      })))
    }
  }
  groups.push({ id: 'host', title: text('host'), rows: hostRows })
  if (runtimeRows.length) groups.push({ id: 'runtime', title: text('runtime'), rows: runtimeRows })
  groups.push({ id: 'activity', title: text('activity'), rows: activityRows })
  const findings = groups.flatMap(group => group.rows)
  const unknown = !host || !state || setupError || snapshotError ||
    findings.some(item => item.id.endsWith('unreported'))
  const status = findings.some(item => item.status === 'fail') ? 'fail' :
    findings.some(item => item.status === 'warning') ? 'warning' : disabled || unknown || updating ? 'info' : 'pass'
  const summary = !host || !state || setupError || snapshotError ? 'unverified' : disabled ? 'disabled' :
    status === 'fail' || status === 'warning' ? 'attention' : updating ? 'updating' : unknown ? 'incomplete' : 'checked'
  return { status, summary: text(summary), groups }
}
