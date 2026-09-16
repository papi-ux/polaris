// Commands are fixed application text, never shell instructions returned by an API.
export const hostChecks = ['docker', 'docker_access', 'identity', 'input', 'gpu', 'security']
const checkIds = [...hostChecks, 'runtime', 'spaces']
const runtimeStatuses = ['not_published', 'unsupported', 'available', 'ready', 'failed']

// The gaming runtime check, since Polaris 1.4.9. Its status picks the row; its
// state keeps the grammar every check shares, and only a build without a
// runtime is neutral. A runtime is named only when one fits this PC.
export function validRuntimeCheck(item) {
  const runtime = item?.runtime
  if (!runtime || typeof runtime !== 'object' || !runtimeStatuses.includes(runtime.status) ||
      typeof runtime.code !== 'string' || !/^[a-z0-9_]{1,64}$/.test(runtime.code)) return false
  if (item.state !== (runtime.status === 'ready' ? 'ready' : runtime.status === 'not_published' ? 'not_configured' : 'required')) return false
  if (!['available', 'ready', 'failed'].includes(runtime.status)) return runtime.id === undefined
  return typeof runtime.id === 'string' && /^[a-z0-9][a-z0-9-]{0,63}$/.test(runtime.id) &&
    typeof runtime.nvidia_driver === 'string' && (runtime.variant === 'default' ? runtime.nvidia_driver === '' :
      runtime.variant === 'nvidia' && runtime.nvidia_driver.length <= 32 && /^[0-9]+(?:\.[0-9]+)+$/.test(runtime.nvidia_driver))
}

export function validSetup(value) {
  if (!value || value.version !== 2 || typeof value.distribution !== 'string' ||
      typeof value.immutable_host !== 'boolean' ||
      !Number.isSafeInteger(value.service_uid) || value.service_uid < 0 ||
      !['host_prerequisites_ready', 'configured', 'available'].every(key => typeof value[key] === 'boolean') ||
      !Array.isArray(value.checks)) return false
  // Hosts from before the runtime check send the other seven.
  const withRuntime = value.checks.some(item => item?.id === 'runtime')
  if (value.checks.length !== checkIds.length - (withRuntime ? 0 : 1)) return false
  const seen = new Set()
  for (const item of value.checks) {
    if (!item || !checkIds.includes(item.id) || seen.has(item.id) ||
        !['ready', 'required', 'not_configured'].includes(item.state) ||
        typeof item.title !== 'string' || typeof item.detail !== 'string' || typeof item.action !== 'string' ||
        (item.doc_anchor !== undefined && !/^#[a-z0-9-]{1,64}$/.test(item.doc_anchor))) return false
    if (!['spaces', 'runtime'].includes(item.id) && item.state === 'not_configured') return false
    if (item.id === 'runtime' && !validRuntimeCheck(item)) return false
    seen.add(item.id)
  }
  return value.host_prerequisites_ready === hostChecks.every(id => value.checks.find(item => item.id === id).state === 'ready') &&
    (!value.available || value.configured) &&
    (value.checks.find(item => item.id === 'spaces').state === 'ready') === value.available
}

export const installGuides = {
  fedora: {
    name: 'Fedora',
    url: 'https://docs.docker.com/engine/install/fedora/',
    steps: [
      { title: 'Add the Docker package repository', command: 'sudo dnf config-manager addrepo --from-repofile https://download.docker.com/linux/fedora/docker-ce.repo' },
      { title: 'Install Docker Engine', command: 'sudo dnf install docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin' },
    ],
  },
  arch: {
    name: 'Arch Linux',
    url: 'https://wiki.archlinux.org/title/Docker',
    steps: [{ title: 'Update Arch and install Docker', command: 'sudo pacman -Syu docker' }],
  },
  ubuntu: {
    name: 'Ubuntu',
    url: 'https://docs.docker.com/engine/install/ubuntu/',
    steps: [
      { title: 'Install the repository prerequisites', command: 'sudo apt update\nsudo apt install ca-certificates curl' },
      { title: 'Add the Docker signing key', command: 'sudo install -m 0755 -d /etc/apt/keyrings\nsudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc\nsudo chmod a+r /etc/apt/keyrings/docker.asc' },
      { title: 'Add the Docker package repository', command: 'sudo tee /etc/apt/sources.list.d/docker.sources <<EOF\nTypes: deb\nURIs: https://download.docker.com/linux/ubuntu\nSuites: $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}")\nComponents: stable\nArchitectures: $(dpkg --print-architecture)\nSigned-By: /etc/apt/keyrings/docker.asc\nEOF\nsudo apt update' },
      { title: 'Install Docker Engine', command: 'sudo apt install docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin' },
    ],
  },
}
export const startDocker = 'sudo systemctl enable --now docker'
export function dockerAccessCommand(uid) {
  return Number.isSafeInteger(uid) && uid > 0 && uid <= 2147483647
    ? 'sudo usermod -aG docker -- "$(id -nu -- ' + uid + ')"' : ''
}
export function installGuide(setup) {
  if (!setup || setup.immutable_host) return null
  return Object.hasOwn(installGuides, setup.distribution) ? installGuides[setup.distribution] : null
}

export const installSpacesSecurity = 'sudo -H /usr/bin/polaris-spaces-setup install'
export const fedoraSecurityPackages = 'sudo dnf install selinux-policy-devel container-selinux make'

// The guide on papi-ux.com. A check names its own section (doc_anchor) since
// Polaris 1.4.9; older hosts get the section the console has always linked.
export const docsUrl = 'https://papi-ux.com/docs/spaces/'
const legacyAnchors = {
  docker: '#prepare-docker-from-spaces', docker_access: '#prepare-docker-from-spaces', identity: '#gaming-runtime-account',
  input: '#controller-access', gpu: '#graphics-access', security: '#prepare-spaces-security-support',
  runtime: '#download-the-gaming-runtime', spaces: '#prepare-your-first-space',
}
export function guideHref(check) {
  const anchor = typeof check?.doc_anchor === 'string' && /^#[a-z0-9-]{1,64}$/.test(check.doc_anchor)
    ? check.doc_anchor : legacyAnchors[check?.id] || ''
  return docsUrl + anchor
}

// The terminal steps a failing check needs, in order, from this file's fixed
// text only. Package installs are withheld on an immutable host; starting the
// daemon and granting the service account access apply everywhere.
export function setupSteps(setup, check) {
  if (!setup || !check || check.state === 'ready') return []
  const guide = installGuide(setup)
  if (check.id === 'docker') {
    return [...(guide?.steps || []), { title: 'Start Docker', command: startDocker }]
  }
  if (check.id === 'docker_access') {
    const access = dockerAccessCommand(setup.service_uid)
    return [{ title: 'Start Docker', command: startDocker },
      ...(access ? [{ title: 'Let the Polaris service account use Docker', command: access }] : [])]
  }
  if (check.id === 'security') {
    if (setup.immutable_host) return []
    return [...(setup.distribution === 'fedora' ? [{ title: 'Install the policy tools', command: fedoraSecurityPackages }] : []),
      { title: 'Run the helper from the native package, with Polaris stopped', command: installSpacesSecurity }]
  }
  return []
}
