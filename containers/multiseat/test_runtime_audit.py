"""Reject stale, substituted or incomplete publication scan evidence."""
from copy import deepcopy
from datetime import datetime, timezone, timedelta
import importlib.util
import json
import tempfile
from unittest import mock
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('audit_runtime', Path(__file__).with_name('audit-runtime.py'))
audit = importlib.util.module_from_spec(spec); spec.loader.exec_module(audit)


class RuntimeAudit(unittest.TestCase):
    def test_exact_subject_current_database_and_both_scopes_are_required(self):
        now = datetime(2026, 9, 14, 12, tzinfo=timezone.utc)
        artifact = {'worker_digest': 'sha256:' + 'a' * 64, 'worker_config_digest': 'sha256:' + 'b' * 64}
        tools = {'syft': {'version': '1.51.1'}, 'grype': {'version': '0.118.0'}}
        base_inventory = {'descriptor': {'name': 'syft', 'version': '1.51.1'}, 'artifacts': [{'type': 'deb'}],
            'source': {'type': 'image', 'metadata': {'imageID': artifact['worker_config_digest'],
                                                  'manifestDigest': artifact['worker_digest']}}}
        base_report = {'descriptor': {'name': 'grype', 'version': '0.118.0', 'db': {'status': {
            'built': (now - timedelta(hours=1)).isoformat(), 'valid': True,
            'schemaVersion': 'v6.1.9', 'from': 'https://example.invalid/db?checksum=sha256:abc'}}},
            'matches': [], 'ignoredMatches': []}
        for case in ('valid', 'high', 'critical-declared', 'stale', 'future', 'invalid', 'different-db',
                     'foreign-config', 'foreign-manifest', 'wrong-scanner', 'empty', 'ignored', 'missing-scope'):
            with self.subTest(case=case):
                inventory = deepcopy(base_inventory)
                reports = {scope: deepcopy(base_report) for scope in ('image', 'declared')}
                status = reports['image']['descriptor']['db']['status']
                if case == 'high': reports['image']['matches'] = [{'vulnerability': {'severity': 'High'}}]
                elif case == 'critical-declared': reports['declared']['matches'] = [{'vulnerability': {'severity': 'Critical'}}]
                elif case == 'stale': status['built'] = (now - timedelta(days=6)).isoformat()
                elif case == 'future': status['built'] = (now + timedelta(hours=1)).isoformat()
                elif case == 'invalid': status['valid'] = False
                elif case == 'different-db': status['from'] += 'other'
                elif case == 'foreign-config': inventory['source']['metadata']['imageID'] = 'other'
                elif case == 'foreign-manifest': inventory['source']['metadata']['manifestDigest'] = 'other'
                elif case == 'wrong-scanner': reports['image']['descriptor']['version'] = 'old'
                elif case == 'empty': inventory['artifacts'] = []
                elif case == 'ignored': reports['image']['ignoredMatches'] = [{'vulnerability': {'severity': 'High'}}]
                elif case == 'missing-scope': del reports['declared']
                if case in ('valid', 'high', 'critical-declared'):
                    result = audit.assess(artifact, inventory, reports, tools, now)
                    self.assertEqual(result['result'], 'passed' if case == 'valid' else 'blocked')
                else:
                    with self.assertRaises(ValueError): audit.assess(artifact, inventory, reports, tools, now)

    def test_signing_rechecks_receipt_hashes_and_severity(self):
        now = datetime.now(timezone.utc)
        tools = json.loads((audit.HERE / 'locks/scanners.json').read_text())
        revision = 'a' * 40
        artifact = {'worker_digest': 'sha256:' + 'b' * 64, 'worker_config_digest': 'sha256:' + 'c' * 64}
        inventory = {'descriptor': {'name': 'syft', 'version': tools['syft']['version']}, 'artifacts': [{}],
            'source': {'type': 'image', 'metadata': {'imageID': artifact['worker_config_digest'],
                'manifestDigest': artifact['worker_digest']}}}
        scan = {'descriptor': {'name': 'grype', 'version': tools['grype']['version'], 'db': {'status': {
            'built': (now - timedelta(hours=1)).isoformat(), 'valid': True,
            'schemaVersion': 'v6', 'from': 'https://example.invalid/db'}}}, 'matches': []}
        for case in ('valid', 'input', 'output', 'receipt-source', 'scanner-lock', 'summary', 'rehash-critical'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); evidence = root / 'audit'; evidence.mkdir()
                for name in ('artifact.json', 'worker.oci.tar', 'sbom.cdx.json'):
                    (root / name).write_text('fixture')
                for name, value in [('image-sbom.syft.json', inventory), ('image-sbom.cdx.json', {}),
                                    ('image-vulnerabilities.json', scan), ('declared-vulnerabilities.json', scan)]:
                    (evidence / name).write_text(json.dumps(value))
                receipt = audit.assess(artifact, inventory, {'image': scan, 'declared': scan}, tools)
                receipt.update(schema=1, scanners=tools, source_revision=revision,
                    worker_digest=artifact['worker_digest'], worker_config_digest=artifact['worker_config_digest'],
                    artifact_sha256=audit.checksum(root / 'artifact.json'),
                    inputs={name: audit.checksum(root / name) for name in ('worker.oci.tar', 'sbom.cdx.json')},
                    outputs={path.name: audit.checksum(path) for path in evidence.iterdir()})
                if case == 'input': (root / 'worker.oci.tar').write_text('substitution')
                elif case == 'output': (evidence / 'image-vulnerabilities.json').write_text('{}')
                elif case == 'receipt-source': receipt['source_revision'] = 'e' * 40
                elif case == 'scanner-lock': receipt['scanners'] = {}
                elif case == 'summary': receipt['match_counts']['image'] = {'High': 0}
                elif case == 'rehash-critical':
                    altered = deepcopy(scan); altered['matches'] = [{'vulnerability': {'severity': 'Critical'}}]
                    (evidence / 'declared-vulnerabilities.json').write_text(json.dumps(altered))
                    receipt['outputs']['declared-vulnerabilities.json'] = audit.checksum(evidence / 'declared-vulnerabilities.json')
                (evidence / 'audit.json').write_text(json.dumps(receipt))
                # OCI validation is covered independently; this exercises the signing receipt boundary.
                with mock.patch.object(audit, 'subject', return_value=artifact):
                    if case == 'valid': self.assertEqual(audit.verify_receipt(root, revision)['result'], 'passed')
                    else:
                        with self.assertRaises(ValueError): audit.verify_receipt(root, revision)


if __name__ == '__main__':
    unittest.main()
