#!/usr/bin/python3 -I
"""Destructive acceptance checks, permitted only in an explicitly marked test VM."""
import argparse
import fcntl
import hashlib
import importlib.machinery
import importlib.util
import json
import os
import pwd
import re
from pathlib import Path
import signal
import struct
import subprocess
import sys
import time

MARKER = Path('/etc/polaris-spaces-disposable-test')
TOKEN = 'polaris-spaces-security-acceptance-v1\n'


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def command(args, expected=0, timeout=150):
    result = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    check(result.returncode == expected, str(args) + ': ' + result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', required=True, type=Path)
    parser.add_argument('--reader', default='spacescheck')
    parser.add_argument('--expected-helper-sha256', required=True)
    parser.add_argument('--source-commit', required=True)
    args = parser.parse_args()
    check(re.fullmatch('[0-9a-f]{64}', args.expected_helper_sha256) and
          re.fullmatch('[0-9a-f]{40}', args.source_commit), 'Require exact candidate and source identities.')
    check(os.geteuid() == 0, 'Run inside the disposable VM as root.')
    check(MARKER.is_file() and not MARKER.is_symlink() and MARKER.stat().st_uid == 0 and
          not MARKER.stat().st_mode & 0o022 and MARKER.read_text() == TOKEN, 'Missing explicit disposable-VM marker.')
    check(command(['systemd-detect-virt', '--vm']).strip() in ('kvm', 'qemu'), 'Requires a QEMU test VM.')
    check(command(['getenforce']).strip() == 'Enforcing', 'Requires SELinux enforcing throughout.')
    check(pwd.getpwnam(args.reader).pw_uid != 0, 'Readiness must be checked as a normal user.')
    helper = args.helper.resolve(strict=True)
    check(helper.name == 'polaris-spaces-setup' and helper.stat().st_uid == 0 and
          not helper.stat().st_mode & 0o022, 'Requires the root-owned packaged helper.')
    check(hashlib.sha256(helper.read_bytes()).hexdigest() == args.expected_helper_sha256, 'Installed helper differs from the expected candidate.')
    loader = importlib.machinery.SourceFileLoader('spaces_candidate', str(helper))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec); loader.exec_module(module)
    system = module.System()
    check(system.modules() == {}, 'Start with no installed Spaces modules.')
    check(not module.RULE.exists(), 'Start with no live Spaces input rule.')
    evidence = {'source_commit': args.source_commit, 'kernel': os.uname().release,
                'selinux_packages': command(['rpm', '-q', 'selinux-policy', 'selinux-policy-devel', 'container-selinux', 'policycoreutils']).splitlines(),
                'helper_sha256': hashlib.sha256(helper.read_bytes()).hexdigest(),
                'policy_release': module.RELEASE, 'passed': []}

    def passed(name):
        check(command(['getenforce']).strip() == 'Enforcing', 'SELinux enforcement changed.')
        evidence['passed'].append(name)
        print('PASS: ' + name, flush=True)

    def invoke(action, expected=0):
        return command([str(helper), action], expected)

    def status(ready):
        result = command(['runuser', '-u', args.reader, '--', str(helper), 'status'], 0 if ready else 1)
        check(json.loads(result)['ready'] is ready, 'Wrong unprivileged readiness result.')

    def state():
        return module.decode_state((module.STATE / 'state.json').read_bytes())

    def interrupted(action):
        phase = 'installing' if action == 'install' else 'removing'
        log = Path('/var/tmp/spaces-interrupted-' + action + '.log')
        with log.open('w') as output:
            process = subprocess.Popen([str(helper), action], stdout=output, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            stopped = False
            try:
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        children = Path('/proc/' + str(process.pid) + '/task/' + str(process.pid) + '/children').read_text().split()
                        semodule = any(Path('/proc/' + pid + '/comm').read_text().strip() == 'semodule' for pid in children)
                        if state()['phase'] == phase and semodule:
                            os.kill(process.pid, signal.SIGSTOP)
                            stopped = True
                            break
                    except FileNotFoundError:
                        pass
                    time.sleep(0.005)
                check(stopped, 'Could not intercept the installer during its policy transaction: ' + log.read_text())
                expected = state()['after'] if action == 'install' else {}
                deadline = time.monotonic() + 60
                while system.modules() != expected and time.monotonic() < deadline:
                    time.sleep(0.05)
                check(system.modules() == expected, 'Policy transaction did not commit while its caller was paused.')
                check(state()['phase'] == phase, 'Caller completed before interruption.')
                os.kill(process.pid, signal.SIGKILL); process.wait(timeout=5)
            finally:
                if process.poll() is None:
                    os.kill(process.pid, signal.SIGKILL); process.wait(timeout=5)
        check(not (module.STATE / 'ready.json').exists(), 'Interrupted change left a ready receipt.')
        invoke(action)
        check(state()['phase'] == ('installed' if action == 'install' else 'removed'), 'Retry did not finish the saved operation.')
        status(action == 'install')
        passed('resume ' + action + ' after a real policy commit and killed installer')

    status(False)
    invoke('install'); status(True)
    installed = system.modules()
    check(installed == state()['after'], 'Installed policy checksums differ from the journal.')
    invoke('install')
    check(system.modules() == installed, 'Idempotent install changed the policy.')
    for name in ('state.json', 'polaris_nvidia_worker.cil'):
        check((module.STATE / name).stat().st_mode & 0o077 == 0, 'Private setup file is accessible to other users.')
    passed('real install, exact policy read-back, idempotency and unprivileged readiness')

    active = subprocess.Popen([sys.executable, '-c', 'import ctypes,time; ctypes.CDLL(None).prctl(15,b"polaris",0,0,0); print("ready",flush=True); time.sleep(60)'], stdout=subprocess.PIPE, text=True)
    try:
        check(active.stdout.readline().strip() == 'ready', 'Activity fixture failed.')
        check('Quit Polaris' in invoke('remove', 1), 'Removal did not refuse an active Polaris process.')
        check(system.modules() == installed, 'Active-process refusal changed policy.')
    finally:
        active.terminate(); active.wait(timeout=5); active.stdout.close()
    passed('active Polaris process prevents removal')

    command(['modprobe', 'uinput'])
    devices = []
    try:
        for name in ('Polaris multiseat validation-reserved', 'Spaces validation ordinary'):
            fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            devices.append(fd)
            fcntl.ioctl(fd, 0x40045564, 1)  # UI_SET_EVBIT, EV_KEY
            fcntl.ioctl(fd, 0x40045565, 304)  # UI_SET_KEYBIT, BTN_SOUTH
            payload = struct.pack('HHHH80sI', 3, 0x1234, 0x5678, 1, name.encode(), 0)
            fcntl.ioctl(fd, 0x405c5503, payload)  # UI_DEV_SETUP
            fcntl.ioctl(fd, 0x5501)  # UI_DEV_CREATE
        command(['udevadm', 'settle', '--timeout=10'])
        found = {}
        for path in Path('/sys/class/input').glob('event*/device/name'):
            name = path.read_text().strip()
            if name in ('Polaris multiseat validation-reserved', 'Spaces validation ordinary'):
                node = Path('/dev/input') / path.parents[1].name
                found[name] = (node, os.getxattr(node, 'security.selinux').decode().rstrip('\0'),
                               command(['udevadm', 'info', '--query=property', '--name=' + str(node)]))
        check(len(found) == 2, 'Both virtual input devices must be present.')
        reserved, context, properties = found['Polaris multiseat validation-reserved']
        check(context == 'system_u:object_r:polaris_multiseat_input_device_t:s0', 'Reserved input has the wrong label: ' + context)
        check('ID_SEAT=seat-polaris' in properties, 'Reserved input did not receive its seat.')
        ordinary, context, properties = found['Spaces validation ordinary']
        check('polaris_multiseat_input_device_t' not in context and 'ID_SEAT=seat-polaris' not in properties,
              'Ordinary input was incorrectly reserved.')
        probe = """import fcntl,os,sys
try:
    fd=os.open(sys.argv[1], (os.O_WRONLY if sys.argv[2]=='write' else os.O_RDONLY)|os.O_NONBLOCK)
    if sys.argv[2]=='grab':
        fcntl.ioctl(fd,0x40044590,1)
        fcntl.ioctl(fd,0x40044590,0)
    else:
        fcntl.ioctl(fd,0x80044501,bytearray(4),True)
    os.close(fd)
except PermissionError:
    raise SystemExit(13)
"""
        worker = ['runcon', 'system_u:system_r:polaris_nvidia_worker_t:s0', '/usr/bin/python3', '-I', '-c', probe]
        command(worker + [str(reserved), 'read'])
        command(worker + [str(reserved), 'write'], 13)
        command(worker + [str(reserved), 'grab'], 13)
        command(worker + [str(ordinary), 'read'], 13)
        # Prove the driver's grab operation itself works for the guest root.
        command(['/usr/bin/python3', '-I', '-c', probe, str(reserved), 'grab'])
        check('Quit Polaris' in invoke('remove', 1), 'Active input did not prevent removal.')
    finally:
        for fd in devices:
            os.close(fd)
        command(['udevadm', 'settle', '--timeout=10'])
    passed('reserved input labeling, worker read access, and denied write, grab and ordinary input access')

    original = module.RULE.read_bytes()
    try:
        module.RULE.write_bytes(original + b'\n# independent local change\n')
        status(False)
        check('not owned' in invoke('remove', 1), 'A changed rule was silently removed.')
        check(system.modules() == installed, 'Rule refusal changed installed policy.')
    finally:
        module.RULE.write_bytes(original)
    status(True)
    invoke('remove'); status(False)
    check(system.modules() == {} and not module.kernel_ready(), 'Removal left Spaces policy loaded.')
    passed('changed input rules are preserved; owned removal clears running policy')

    candidate = module.DATA / 'polaris_multiseat_input.cil'
    permissions = candidate.stat().st_mode & 0o777
    try:
        candidate.chmod(0o666)
        check('unsafe' in invoke('install', 1), 'Writable packaged policy was accepted.')
        check(system.modules() == {}, 'Unsafe source refusal changed policy.')
    finally:
        candidate.chmod(permissions)
    passed('writable policy source is rejected before installation')

    command(['/usr/bin/semodule', '-X', '400', '-i', str(candidate)])
    try:
        before = command(['/usr/bin/semodule', '-lfull', '-m'])
        check('locally managed' in invoke('install', 1), 'Manual policy was adopted.')
        check(command(['/usr/bin/semodule', '-lfull', '-m']) == before, 'Manual policy changed.')
    finally:
        command(['/usr/bin/semodule', '-X', '400', '-r', 'polaris_multiseat_input'])
    passed('existing administrator policy is preserved without adoption')

    interrupted('install')
    interrupted('remove')
    invoke('install'); status(True)
    Path('/var/tmp/spaces-security-acceptance.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence), flush=True)
    print('Installed state retained for a guest reboot and unprivileged status check.', flush=True)


if __name__ == '__main__':
    main()
