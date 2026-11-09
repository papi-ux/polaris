#!/usr/bin/python3 -I
"""Fedora release-asset updater. No resolver, shell, caller URL or caller path.

plan/status are unprivileged reads; install is a separately authorized polkit
action. Its stdin is ONLY a reviewed-identity equality guard. All root inputs
to downloads and RPM are independently derived here from official metadata.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import selectors
import ssl
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

API = 'https://api.github.com/repos/papi-ux/polaris/releases?per_page=100'
UPSTREAM = 'https://github.com/papi-ux/polaris/releases/download/'
ENV = {'PATH': '/usr/sbin:/usr/bin', 'LANG': 'C', 'LC_ALL': 'C', 'HOME': '/root'}
STATE = Path('/var/lib/polaris/update')
RPM = '/usr/bin/rpm'
PACKAGES = ('polaris', 'polaris-kms')
# The Fedora releases whose official release assets this helper installs, by exact VERSION_ID.
# A host outside this set, a derivative, another architecture or an image based system is refused.
FEDORA_RELEASES = ('44', '45')
FAMILY_MESSAGE = 'Polaris can install updates on Fedora 44 or 45 x86_64 only. Use the guided steps for this system.'
VERSION = re.compile(r'(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})(?:-(beta|rc)\.([1-9][0-9]{0,5}))?\Z')
STARTED = 'Polaris update started.'
MAX_METADATA = 4 * 1024 * 1024
MAX_ASSET = 512 * 1024 * 1024


class Refusal(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def require(condition, code, message):
    if not condition:
        raise Refusal(code, message)


def arguments(argv):
    require(len(argv) in (1, 2, 3), 'invalid_request', 'Invalid update arguments.')
    action = argv[0]
    require(action in ('status', 'plan', 'install'), 'invalid_request', 'Unknown update action.')
    if action == 'status':
        require(len(argv) == 1, 'invalid_request', 'Status takes no arguments.')
        return action, '', ''
    require(len(argv) == (2 if action == 'plan' else 3), 'invalid_request', 'Invalid update arguments.')
    channel = argv[1]
    version = argv[2] if action == 'install' else ''
    require(channel in ('stable', 'beta'), 'invalid_request', 'Unknown release channel.')
    require(not version or bool(VERSION.fullmatch(version)), 'invalid_request', 'Invalid release version.')
    require(action != 'install' or bool(version), 'invalid_request', 'A reviewed version is required.')
    return action, channel, version


def version_key(value):
    match = VERSION.fullmatch(value)
    require(bool(match), 'release_unverifiable', 'Release version is not recognized.')
    major, minor, patch, label, number = match.groups()
    return (int(major), int(minor), int(patch), {'beta': 0, 'rc': 1, None: 2}[label], int(number or 0))


def qualify_host(os_release=None, machine=None, image_based=None):
    if os_release is None:
        os_release = Path('/etc/os-release').read_text()
    values = {}
    for line in os_release.splitlines():
        key, separator, value = line.partition('=')
        if separator:
            values[key] = value.strip().strip('"\'')
    if image_based is None:
        image_based = Path('/run/ostree-booted').exists() or Path('/usr/bin/steamos-readonly').exists()
    require(not image_based, 'image_based_host', 'Image based and SteamOS updates are still guided.')
    version_id = values.get('VERSION_ID', '')
    require(values.get('ID') == 'fedora' and version_id in FEDORA_RELEASES
            and (machine or platform.machine()) == 'x86_64', 'family_unqualified', FAMILY_MESSAGE)
    return {'id': 'fedora', 'version_id': version_id, 'arch': 'x86_64'}


def qualified_host(host):
    """The exact host identity qualify_host produced; any other value is an untrusted hint."""
    require(isinstance(host, dict) and set(host) == {'id', 'version_id', 'arch'} and host['id'] == 'fedora'
            and host['version_id'] in FEDORA_RELEASES and host['arch'] == 'x86_64', 'family_unqualified', FAMILY_MESSAGE)
    return {'id': 'fedora', 'version_id': host['version_id'], 'arch': 'x86_64'}


class OfficialRedirect(urllib.request.HTTPRedirectHandler):
    """Only GitHub's official asset delivery hosts, always system-CA HTTPS."""
    def redirect_request(self, request, fp, code, message, headers, newurl):
        url = urllib.parse.urlsplit(newurl)
        require(url.scheme == 'https' and url.hostname in
                ('github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com')
                and not url.username and not url.password and url.port in (None, 443),
                'release_unverifiable', 'Release download redirected outside official HTTPS delivery.')
        return super().redirect_request(request, fp, code, message, headers, newurl)


def open_official(url):
    # Ignore caller proxy/TLS/environment settings, even when invoked without pkexec.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), OfficialRedirect(),
                                         urllib.request.HTTPSHandler(context=ssl.create_default_context(
                                             cafile='/etc/pki/tls/certs/ca-bundle.crt')))
    return opener.open(urllib.request.Request(url, headers={'User-Agent': 'Polaris-host-update',
                                                          'Accept': 'application/vnd.github+json'}), timeout=30)


def releases():
    with open_official(API) as response:
        raw = response.read(MAX_METADATA + 1)
    require(len(raw) <= MAX_METADATA, 'release_unverifiable', 'Official release metadata is too large.')
    data = json.loads(raw)
    require(isinstance(data, list), 'release_unverifiable', 'Official release metadata is unavailable.')
    return data


def select_release(data, channel, installed, host):
    host = qualified_host(host)
    # Both package assets are named after the actual host release; there is no other-release fallback.
    suffix = 'fedora' + host['version_id'] + '-x86_64.rpm'
    candidates = []
    for entry in data:
        if not isinstance(entry, dict) or entry.get('draft') is not False:
            continue
        tag = entry.get('tag_name', '')
        if not isinstance(tag, str) or not tag.startswith('v') or not VERSION.fullmatch(tag[1:]):
            continue
        version = tag[1:]
        prerelease = '-' in version
        # Include beta means stable plus prereleases, preserving the existing console preference.
        if entry.get('prerelease') is not prerelease or (channel == 'stable' and prerelease):
            continue
        candidates.append((version_key(version), entry))
    require(bool(candidates), 'release_unverifiable', 'No official release is available on this channel.')
    candidates.sort(key=lambda candidate: candidate[0], reverse=True)
    entry = candidates[0][1]
    require(sum(item[0] == candidates[0][0] for item in candidates) == 1,
            'release_unverifiable', 'Official release identity is ambiguous.')
    version = entry['tag_name'][1:]
    require(type(entry.get('id')) is int and entry['id'] > 0,
            'release_unverifiable', 'Official release identity is missing.')
    selected = []
    for name in PACKAGES:
        if name == 'polaris-kms' and installed[name]['state'] == 'absent':
            continue
        wanted = ('Polaris-' if name == 'polaris' else 'Polaris-kms-') + suffix
        assets = [asset for asset in entry.get('assets', []) if isinstance(asset, dict) and asset.get('name') == wanted]
        require(len(assets) == 1, 'release_unverifiable', 'A matching official package asset is missing or ambiguous.')
        asset = assets[0]
        digest = asset.get('digest', '')
        require(isinstance(digest, str) and bool(re.fullmatch(r'sha256:[0-9a-f]{64}', digest)),
                'release_unverifiable', 'The release has no verifiable SHA-256 for this asset.')
        require(type(asset.get('id')) is int and asset['id'] > 0 and type(asset.get('size')) is int
                and 0 < asset['size'] <= MAX_ASSET, 'release_unverifiable', 'Official asset identity is missing.')
        # Construct the only allowed URL; even official metadata cannot introduce another endpoint.
        url = UPSTREAM + entry['tag_name'] + '/' + wanted
        require(asset.get('browser_download_url') == url, 'release_unverifiable', 'Unexpected official asset URL.')
        selected.append({'package': name, 'id': asset['id'], 'name': wanted, 'digest': digest,
                         'size': asset['size'], 'url': url, 'updated_at': asset.get('updated_at', '')})
    identity = {'schema': 1, 'release_id': entry['id'], 'tag': entry['tag_name'], 'channel': channel,
                'host': host, 'installed': installed, 'assets': selected}
    fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    # Derive from the selected entry, not the last candidate visited above.
    kind = 'rc' if '-rc.' in version else 'beta' if '-' in version else 'stable'
    return {'channel': channel, 'release_kind': kind, 'version': version, 'release_id': entry['id'], 'assets': selected,
            'host': host, 'review_guard': fingerprint, 'trust': 'release_digest',
            'release_url': 'https://github.com/papi-ux/polaris/releases/tag/' + entry['tag_name']}


def command(argv, timeout=30):
    # No transaction timeout: killing RPM after approval is unsafe. Read queries are bounded.
    with subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, env=ENV, cwd='/') as process:
        deadline = None if timeout is None else time.monotonic() + timeout
        output = bytearray()
        truncated = False
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while selector.get_map():
                if deadline is not None and time.monotonic() >= deadline:
                    process.kill()  # only read/--test operations have a deadline
                    raise Refusal('metadata_unreadable', 'Package metadata query timed out.')
                for key, _ in selector.select(0.1):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                    else:
                        output.extend(chunk)
                        if len(output) > 1024 * 1024:
                            truncated = True
                            del output[:-1024 * 1024]
        code = process.wait()
    require(not truncated, 'metadata_unreadable', 'Package manager output exceeded the bound.')
    return code, output.decode('utf-8', errors='replace')


QUERY = '%{NAME}|%{EPOCHNUM}|%{VERSION}|%{RELEASE}|%{ARCH}\n'


def parse_identity(text, expected):
    lines = text.strip().splitlines()
    require(len(lines) == 1, 'package_unqualified', 'Package identity is missing or ambiguous.')
    fields = lines[0].split('|')
    require(len(fields) == 5 and fields[0] == expected and fields[1] == '0'
            and fields[4] == 'x86_64' and bool(re.fullmatch(r'[0-9A-Za-z.+~_]+', fields[3])),
            'package_unqualified', 'Unexpected package name, epoch, release or architecture.')
    version = fields[2].replace('~', '-')
    require(bool(VERSION.fullmatch(version)), 'package_unqualified', 'Unexpected package version.')
    return {'state': 'installed', 'version': version, 'rpm_version': fields[2], 'release': fields[3], 'arch': fields[4]}


def installed_packages(run=command):
    result = {}
    for name in PACKAGES:
        try:
            code, output = run([RPM, '-q', '--qf', QUERY, '--', name])
            if code == 1 and output.strip() == f'package {name} is not installed':
                result[name] = {'state': 'absent'}
            elif code == 0:
                lines = output.strip().splitlines()
                if len(lines) > 1:
                    # An interrupted upgrade can leave duplicate old/new RPM entries.
                    # Do not select one and pretend the pair is fully installed.
                    identities = [parse_identity(line, name) for line in lines]
                    result[name] = {'state': 'partial', 'versions': [item['version'] for item in identities]}
                else:
                    result[name] = parse_identity(output, name)
            else:
                result[name] = {'state': 'unknown'}
        except Exception:
            result[name] = {'state': 'unknown'}
    return result


def make_plan(channel, *, fetch=releases, read_installed=installed_packages, host=qualify_host):
    qualified = qualified_host(host())
    current = read_installed()
    require(current['polaris']['state'] == 'installed'
            and current['polaris-kms']['state'] in ('installed', 'absent'),
            'installed_state_unknown',
            'Polaris cannot safely update this package state. Read the versions below. '
            'If there are duplicate entries or an unreadable package database, ask your system administrator '
            'or Fedora software manager to repair them, then review again. Polaris will not force a reinstall or remove entries.')
    plan = select_release(fetch(), channel, current, qualified)
    plan['installed'] = current
    target = version_key(plan['version'])
    require(all(version_key(value['version']) <= target for value in current.values() if value['state'] == 'installed'),
            'downgrade_unsupported', 'This channel would downgrade an installed package. Downgrades are not enabled.')
    # A mixed pair can be repaired by upgrading only its older member. Never reinstall
    # an already-current member to get RPM past its own checks.
    plan['packages_to_update'] = [name for name, value in current.items()
                                  if value['state'] == 'installed' and version_key(value['version']) < target]
    plan['update_needed'] = bool(plan['packages_to_update'])
    return plan


def download(asset, destination):
    digest = hashlib.sha256()
    count = 0
    with open_official(asset['url']) as response, destination.open('xb') as output:
        os.chmod(destination, 0o600)
        while chunk := response.read(1024 * 1024):
            count += len(chunk)
            require(count <= asset['size'], 'release_unverifiable', 'Package download exceeded its official size.')
            output.write(chunk)
            digest.update(chunk)
    require(count == asset['size'] and 'sha256:' + digest.hexdigest() == asset['digest'],
            'release_unverifiable', 'Package does not match the release SHA-256 and size.')


def validate_package(path, asset, version, run=command):
    code, output = run([RPM, '-qp', '--nomanifest', '--qf', QUERY, '--', str(path)])
    require(code == 0, 'package_unqualified', 'RPM could not read package metadata.')
    identity = parse_identity(output, asset['package'])
    require(identity['version'] == version and identity['rpm_version'] == version.replace('-', '~')
            and identity['release'] == '1', 'release_changed',
            'The package version or RPM release differs from the reviewed release format.')
    code, output = run([RPM, '-qp', '--nomanifest', '--qf', '[%{OBSOLETENAME}\n]', '--', str(path)])
    # Reject ALL obsoletes, including the allowed pair: upgrade by exact name only.
    require(code == 0 and not output.strip(), 'transaction_not_closed', 'Packages with obsoletes are not allowed.')


def transaction_argv(paths, test=False):
    # RPM is deliberately NOT a dependency resolver. Both dependency and file
    # conflict checks stay on in the actual locked transaction. No --oldpackage,
    # --replacefiles, --nodeps, --force, scriptlets or triggers. Only same-name
    # old versions can be removed; incoming obsoletes have already been refused.
    return [RPM, '-U', '--nomanifest', '--noscripts', '--notriggers', *(['--test'] if test else []), '--', *map(str, paths)]


def install(channel, version, guard, directory, *, plan=make_plan, fetch_asset=download,
            run=command, read_installed=installed_packages, progress=lambda stage: None):
    attempted = False
    stage = 'checking_release'
    result = {'state': 'failed', 'stage': stage, 'transaction_attempted': False}
    try:
        require(bool(re.fullmatch(r'[0-9a-f]{64}', guard)), 'invalid_review', 'Review this update again.')
        reviewed = plan(channel)
        require(reviewed['version'] == version and reviewed['review_guard'] == guard,
                'release_changed', 'This release or installed package state changed. Review again.')
        require(reviewed['update_needed'], 'already_installed', 'This version is already installed.')
        paths = []
        for asset in reviewed['assets']:
            if asset['package'] not in reviewed['packages_to_update']:
                continue
            stage = 'downloading'
            progress(stage)
            path = directory / asset['name']  # name came from our own exact family table
            fetch_asset(asset, path)
            stage = 'verifying'
            progress(stage)
            validate_package(path, asset, version, run)
            paths.append(path)
        # Refuse a release replaced during the downloads, before invoking RPM.
        stage = 'checking_release'
        progress(stage)
        require(plan(channel)['review_guard'] == guard, 'release_changed', 'This release changed. Review again.')
        stage = 'checking_transaction'
        progress(stage)
        code, output = run(transaction_argv(paths, test=True))
        require(code == 0, 'transaction_not_closed',
                'RPM stopped before installing. Read its details below; package state or dependencies may need '
                'manual repair. Polaris will not force a reinstall, remove packages or update the system. '
                'After correcting the reported problem, review again. ' + output[-2048:])
        stage = 'installing'
        progress(stage)
        attempted = True
        # This actual operation repeats RPM's checks under its own transaction
        # lock and cannot resolve or add a third package, regardless of preview.
        code, output = run(transaction_argv(paths), timeout=None)
        result.update(state='installed' if code == 0 else 'failed', code='installed' if code == 0 else 'rpm_failed',
                      message='Packages installed. Choose Finish when play has ended.' if code == 0 else
                              'RPM stopped. Read the installed state below, then review again.',
                      detail=output[-2048:])
    except Refusal as error:
        result.update(code=error.code, message=str(error))
    except Exception:
        result.update(code='update_failed', message='Polaris could not complete this update. Review again.')
    finally:
        # Even an exception after starting RPM is not evidence that old versions survived.
        result.update(stage=stage, transaction_attempted=attempted, installed=read_installed())
        if result['state'] == 'installed':
            selected = reviewed['assets']
            if any(result['installed'].get(asset['package'], {}).get('version') != version for asset in selected):
                result.update(state='failed', code='installed_state_unknown',
                              message='RPM returned success, but the installed package state could not be confirmed.')
        result['version'] = version
        result['channel'] = channel
    return result


def trusted_directory(path):
    for component in [*reversed(path.parents), path]:
        meta = component.lstat()
        require(stat.S_ISDIR(meta.st_mode) and meta.st_uid == 0 and not meta.st_mode & 0o022,
                'unsafe_state', 'Update state directory has unsafe ownership or permissions.')


def read_review_guard(source):
    raw = source.read(66)
    require(len(raw) == 65 and bool(re.fullmatch(r'[0-9a-f]{64}\n', raw)),
            'invalid_review', 'Review this update again.')
    return raw[:-1]


def write_status(value):
    fd, temporary = tempfile.mkstemp(prefix='.status-', dir=STATE)
    try:
        with os.fdopen(fd, 'w') as output:
            os.fchmod(output.fileno(), 0o644)
            json.dump(value, output, separators=(',', ':'))
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, STATE / 'status.json')
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def status():
    current = installed_packages()
    try:
        trusted_directory(STATE)
        fd = os.open(STATE / 'status.json', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd) as source:
            meta = os.fstat(source.fileno())
            require(stat.S_ISREG(meta.st_mode) and meta.st_uid == 0 and not meta.st_mode & 0o022,
                    'unsafe_state', 'Untrusted update status.')
            raw = source.read(16385)
        require(len(raw) <= 16384, 'unsafe_state', 'Update status is too large.')
        value = json.loads(raw)
        # Readback, rather than a journal's stale success, is authoritative after a restart.
        value['installed'] = current
        return value
    except FileNotFoundError:
        return {'state': 'idle', 'installed': current}


def main(argv):
    action, channel, version = arguments(argv)
    if action == 'status':
        return status()
    if action == 'plan':
        return make_plan(channel)
    require(os.geteuid() == 0, 'authorization_required', 'Approve this update on the host desktop.')
    # Only the non-sensitive status/lock are readable; downloads remain 0700/0600.
    os.umask(0o022)
    os.environ.clear()
    os.environ.update(ENV)
    os.chdir('/')
    # Create one component at a time and validate before traversing it.
    for path in (Path('/var/lib/polaris'), STATE):
        path.mkdir(mode=0o755, exist_ok=True)
        trusted_directory(path)
    lock_fd = os.open(STATE / 'lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o644)
    with os.fdopen(lock_fd, 'r+') as lock:
        meta = os.fstat(lock.fileno())
        require(stat.S_ISREG(meta.st_mode) and meta.st_uid == 0 and not meta.st_mode & 0o022 and meta.st_nlink == 1,
                'unsafe_state', 'Unsafe update lock.')
        os.fchmod(lock.fileno(), 0o644)
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise Refusal('update_running', 'Another privileged update is still running.') from None
        print(STARTED, flush=True)
        guard = read_review_guard(sys.stdin)
        def progress(stage):
            value = {'state': 'running', 'stage': stage, 'version': version, 'channel': channel}
            write_status(value)
            print(json.dumps(value, separators=(',', ':')), flush=True)
        progress('checking_release')
        # No caller filename, directory, URL or plan file is ever opened by root.
        with tempfile.TemporaryDirectory(prefix='.download-', dir=STATE) as temporary:
            result = install(channel, version, guard, Path(temporary), progress=progress)
        write_status(result)
        return result


if __name__ == '__main__':
    try:
        result = main(sys.argv[1:])
    except Refusal as error:
        result = {'state': 'failed', 'code': error.code, 'message': str(error), 'transaction_attempted': False}
    except Exception:
        result = {'state': 'failed', 'code': 'update_unavailable', 'message': 'Host update information is unavailable.'}
    print(json.dumps(result, separators=(',', ':')), flush=True)
    sys.exit(1 if result.get('state') == 'failed' else 0)
