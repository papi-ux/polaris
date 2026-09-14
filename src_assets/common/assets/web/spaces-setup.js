// Commands are fixed application text, never shell instructions returned by an API.
export const hostChecks = ['docker', 'docker_access', 'identity', 'input', 'gpu', 'security']
const checkIds = [...hostChecks, 'spaces']
export function validSetup(value) {
  if (!value || value.version !== 2 || typeof value.distribution !== 'string' ||
      typeof value.immutable_host !== 'boolean' ||
      !Number.isSafeInteger(value.service_uid) || value.service_uid < 0 ||
      !['host_prerequisites_ready', 'configured', 'available'].every(key => typeof value[key] === 'boolean') ||
      !Array.isArray(value.checks) || value.checks.length !== checkIds.length) return false
  const seen = new Set()
  for (const item of value.checks) {
    if (!item || !checkIds.includes(item.id) || seen.has(item.id) ||
        !['ready', 'required', 'not_configured'].includes(item.state) ||
        typeof item.title !== 'string' || typeof item.detail !== 'string' || typeof item.action !== 'string') return false
    if (item.id !== 'spaces' && item.state === 'not_configured') return false
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

export const installSpacesSecurity = 'sudo -H polaris-spaces-setup install'
export const fedoraSecurityPackages = 'sudo dnf install selinux-policy-devel container-selinux make'
