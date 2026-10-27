"""The cross-repository audit must report missing consumers and continue."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('nova_contract', Path(__file__).with_name('check-nova-contract.py'))
contract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contract)


class NovaContractAudit(unittest.TestCase):
    def test_missing_readers_fail_gate_without_hiding_later_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'docs').mkdir()
            (root / 'Reader.kt').write_text('''
object Reader {
    fun present(json: JSONObject) {
        json.optString("value")
    }
    fun later(json: JSONObject) {
        json.optString("unexpected")
    }
}
''')
            objects = {
                name: {'fields': ['value'], 'nova_reader': {
                    'file': file, 'function': function, 'receiver': 'json'}}
                for name, file, function in [
                    ('missing_function', 'Reader.kt', 'removed'),
                    ('aligned', 'Reader.kt', 'present'),
                    ('missing_file', 'Moved.kt', 'present'),
                    ('later_drift', 'Reader.kt', 'later')]
            }
            (root / 'docs/nova-contract.json').write_text(json.dumps({
                'objects': objects,
                'known_drift': {'polaris_sends_nova_never_reads': {'missing_function': ['value']}}}))
            output = io.StringIO()
            with mock.patch('sys.argv', ['check', '--nova', str(root), '--polaris', str(root)]), contextlib.redirect_stdout(output):
                self.assertEqual(contract.main(), 1)
            result = output.getvalue()
            self.assertIn('function not found: removed', result)
            self.assertIn('reader file not found: Moved.kt', result)
            self.assertIn('aligned: Polaris serves 1, Nova reads 1 [ok]', result)
            self.assertIn('Nova reads [unexpected]', result)
            self.assertIn('4 new drift finding(s)', result)

    def test_nested_helpers_and_nullable_receiver_stay_within_reader(self):
        source = '''
    private fun selected(json: JSONObject?) {
        fun helper() { json?.optString("nested") }
        strictBoolean(json, "enabled")
        another.optString("not_this_object")
    }
    private fun sibling(json: JSONObject) { json.optString("not_this_function") }
'''
        self.assertEqual(contract.reads_for_object(source, {'function': 'selected', 'receiver': 'json'}), {'nested', 'enabled'})

    def test_complementary_partial_readers_are_judged_on_their_union(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            findings = contract.report_reads('game', {'fields': ['a', 'b', 'c']},
                [('game/nova_deck', {'a', 'b'}), ('game/nova_deck', {'c'})], 2, True, {}, {})
        self.assertEqual(findings, 0)
        result = output.getvalue()
        self.assertNotIn('never reads', result)
        self.assertIn('game/nova_deck: Polaris serves 3, Nova reads 2 [ok]', result)
        self.assertIn('game: 2 readers together read 3 of 3 served [ok]', result)

    def test_a_served_field_no_reader_reads_is_reported_once(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            findings = contract.report_reads('game', {'fields': ['a', 'b', 'c']},
                [('game/nova_android', {'a'}), ('game/nova_deck', {'b'})], 2, True, {}, {})
        self.assertEqual(findings, 1)
        self.assertEqual(output.getvalue().count('Polaris serves [c] that Nova never reads'), 1)

    def test_mismatched_or_unavailable_reader_still_fails_without_hiding_missing_reads(self):
        source = 'fun read(json: JSONObject) {\n    json.optString("a")\n    json.optString("b")\n}\n'
        reader = {'function': 'read', 'receiver': 'json', 'style': 'kotlin_org_json', 'reads': ['a']}
        with self.assertRaisesRegex(contract.ReaderUnavailableError, 'declared reads differ'):
            contract.declared_reads('nova_android', reader, source, {}, None)
        with self.assertRaisesRegex(contract.ReaderUnavailableError, 'function not found'):
            contract.declared_reads('nova_android', {**reader, 'function': 'gone'}, source, {}, None)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            # The other declared reader was unavailable: there is no union verdict, but a read
            # Polaris never serves still counts.
            findings = contract.report_reads('game', {'fields': ['a', 'b']},
                [('game/nova_android', {'a', 'z'})], 2, False, {}, {})
        self.assertEqual(findings, 1)
        result = output.getvalue()
        self.assertIn('Nova reads [z] that Polaris never serves', result)
        self.assertNotIn('never reads', result)
        self.assertNotIn('readers together', result)


if __name__ == '__main__':
    unittest.main()
