"""Verify an extracted native package without executing its setup helper."""
import ast
import hashlib
from pathlib import Path
import stat
import sys
import xml.etree.ElementTree as ElementTree

POLICY = 'dev.polaris-stream.app.Polaris.policy'
# The only operations Polaris asks polkit for, each bound to one helper operation.
POLICY_ACTIONS = {
    'dev.polaris-stream.app.polaris.spaces-security-install': ('polaris-spaces-setup', 'install'),
    'dev.polaris-stream.app.polaris.spaces-docker-access': ('polaris-spaces-setup', 'docker-access'),
    'dev.polaris-stream.app.polaris.update-install': ('polaris-update-helper', 'install'),
}


def verify_policy(base, prefix):
    """Every action runs only the packaged helper, and every use asks an administrator in a local session."""
    path = base / 'share/polkit-1/actions' / POLICY
    assert path.is_file() and not path.is_symlink(), 'Package must ship ' + str(path)
    assert not path.stat().st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    source = path.read_text()
    assert '<!ENTITY' not in source
    root = ElementTree.fromstring(source)
    assert root.tag == 'policyconfig'
    actions = {}
    for action in root.findall('action'):
        identity = action.get('id')
        assert identity in POLICY_ACTIONS and identity not in actions, identity
        defaults = action.find('defaults')
        assert defaults is not None and [(item.tag, item.text) for item in defaults] == [
            ('allow_any', 'no'), ('allow_inactive', 'no'), ('allow_active', 'auth_admin')], identity
        annotations = {item.get('key'): item.text for item in action.findall('annotate')}
        assert len(action.findall('annotate')) == 2, identity
        assert set(annotations) == {'org.freedesktop.policykit.exec.path', 'org.freedesktop.policykit.exec.argv1'}, identity
        helper, _ = POLICY_ACTIONS[identity]
        assert annotations['org.freedesktop.policykit.exec.path'] == prefix + '/bin/' + helper, identity
        assert (action.findtext('message') or '').strip() and (action.findtext('description') or '').strip(), identity
        actions[identity] = (helper, annotations['org.freedesktop.policykit.exec.argv1'])
    assert actions == POLICY_ACTIONS, actions


def verify(root, prefix='/usr'):
    root = Path(root).resolve()
    assert prefix.startswith('/') and '..' not in Path(prefix).parts
    base = root / prefix.lstrip('/')
    helper = base / 'bin/polaris-spaces-setup'
    assert helper.is_file() and not helper.is_symlink() and helper.stat().st_mode & stat.S_IXUSR
    source = helper.read_text()
    assert source.startswith('#!/usr/bin/python3 -I\n')
    compile(source, str(helper), 'exec')
    update_helper = base / 'bin/polaris-update-helper'
    assert update_helper.is_file() and not update_helper.is_symlink()
    assert update_helper.stat().st_mode & stat.S_IXUSR and not update_helper.stat().st_mode & 0o022
    update_source = update_helper.read_text()
    assert update_source.startswith('#!/usr/bin/python3 -I\n')
    compile(update_source, str(update_helper), 'exec')
    vendor_key = base / 'share/polaris/keys/polaris.gpg'
    assert vendor_key.is_file() and not vendor_key.is_symlink()
    assert not vendor_key.stat().st_mode & 0o022
    assert hashlib.sha256(vendor_key.read_bytes()).hexdigest() == '3387618dcfa246abb5c53abc3a27893791a2de34916fc0954e6195b3a3bb0fe1'
    tree = ast.parse(source)
    values = {}
    for item in tree.body:
        if isinstance(item, ast.Assign) and len(item.targets) == 1 and isinstance(item.targets[0], ast.Name):
            key = item.targets[0].id
            if key in ('RELEASE', 'MARKER', 'EXPECTED'):
                values[key] = ast.literal_eval(item.value)
            if key == 'DATA':
                assert isinstance(item.value, ast.Call) and isinstance(item.value.func, ast.Name) and item.value.func.id == 'Path'
                assert len(item.value.args) == 1 and not item.value.keywords
                values[key] = ast.literal_eval(item.value.args[0])
    data = base / 'share/polaris/multiseat/security'
    assert values['DATA'] == prefix + '/share/polaris/multiseat/security'
    expected_names = {'polaris_multiseat_input.cil', 'polaris_nvidia_worker.te',
                      'polaris_spaces_version.cil', '97-polaris-multiseat-input.rules'}
    assert set(values['EXPECTED']) == expected_names
    for name, checksum in values['EXPECTED'].items():
        path = data / name
        assert path.is_file() and not path.is_symlink()
        assert hashlib.sha256(path.read_bytes()).hexdigest() == checksum, name
    identity = '1:' + ':'.join(values['EXPECTED'][name] for name in (
        'polaris_multiseat_input.cil', 'polaris_nvidia_worker.te', '97-polaris-multiseat-input.rules'))
    assert hashlib.sha256(identity.encode()).hexdigest() == values['RELEASE']
    marker = 'polaris_spaces_' + values['RELEASE'][:32] + '_t'
    assert values['MARKER'] == marker
    assert (data / 'polaris_spaces_version.cil').read_text() == '(type ' + marker + ')\n(roletype object_r ' + marker + ')\n'
    seccomp = list(data.parent.glob('steam-seccomp-*.json'))
    assert seccomp
    for path in seccomp:
        assert path.name == 'steam-seccomp-' + hashlib.sha256(path.read_bytes()).hexdigest() + '.json'
    # Packages ship inert sources. Activation belongs to the explicit helper.
    for path in ('etc/udev/rules.d/97-polaris-multiseat-input.rules',
                 'usr/lib/udev/rules.d/97-polaris-multiseat-input.rules',
                 'var/lib/polaris/spaces-security/ready.json',
                 'var/lib/polaris/update/status.json', 'var/lib/polaris/update/lock'):
        assert not (root / path).exists(), 'Package must not activate ' + path
    verify_policy(base, prefix)
    return values['RELEASE']


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        raise SystemExit('Usage: verify_package.py EXTRACTED_PACKAGE_ROOT [INSTALL_PREFIX]')
    print('Spaces security package verified: ' + verify(*sys.argv[1:]))
