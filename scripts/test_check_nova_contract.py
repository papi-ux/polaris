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


if __name__ == '__main__':
    unittest.main()
