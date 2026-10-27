"""Focused tests for the new generator; no product/native/checker execution."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('generate_polaris_contract',
    Path(__file__).with_name('generate-polaris-contract.py'))
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class ContractGeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=os.environ.get('POLARIS_CONTRACT_TEST_TMPDIR'))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.schema = {'version': 2, 'objects': {'sample': {
            'fields': ['value'], 'generated': True, 'types': {'value': 'integer'},
            'required_present': ['value'],
            'polaris_emitter': {'file': 'sample.cpp', 'function': 'emit',
                'style': 'initializer_list', 'declaration': 'return {'}}}}
        self.write('sample.cpp', 'json emit() { return {{v1::sample::value, 7}}; }\n')
        self.store_schema()

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)

    def store_schema(self):
        self.write(generator.SCHEMA, json.dumps(self.schema, indent=2) + '\n')

    def invoke(self, *arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = generator.main(['--root', str(self.root), *arguments])
        return result, output.getvalue()

    def test_determinism_lf_and_content_bound_provenance(self):
        raw = (self.root / generator.SCHEMA).read_bytes()
        first = generator.render(self.schema, raw, b'generator input')
        second = generator.render(copy.deepcopy(self.schema), bytes(raw), b'generator input')
        self.assertEqual(first, second)
        for data in first.values():
            self.assertNotIn(b'\r', data)
            self.assertTrue(data.endswith(b'\n'))
            self.assertIn(hashlib.sha256(raw).hexdigest().encode(), data)
            self.assertIn(hashlib.sha256(b'generator input').hexdigest().encode(), data)
            self.assertNotIn(str(self.root).encode(), data)
        self.assertNotEqual(first, generator.render(self.schema, raw, b'changed generator input'))

    def test_check_nonzero_for_missing_or_stale_output_and_never_rewrites(self):
        code, output = self.invoke('--check')
        self.assertEqual(code, 1)
        self.assertIn('stale generated output', output)
        self.assertEqual(self.invoke()[0], 0)
        self.assertEqual(self.invoke('--check')[0], 0)
        target = self.root / generator.CPP
        target.write_bytes(target.read_bytes() + b'// stale\n')
        before = target.read_bytes()
        self.assertEqual(self.invoke('--check')[0], 1)
        self.assertEqual(target.read_bytes(), before)

    def test_identifiers_keywords_duplicate_members_and_constant_collisions_refuse(self):
        for fields in (['BadName'], ['while'], ['value', 'value']):
            with self.subTest(fields=fields):
                changed = copy.deepcopy(self.schema)
                changed['objects']['sample']['fields'] = fields
                with self.assertRaises(ValueError):
                    generator.validate(changed)
        with self.assertRaisesRegex(ValueError, 'duplicate schema member'):
            generator.load_schema(b'{"version":2,"version":2,"objects":{}}')

    def test_real_schema_retains_all_objects_and_generates_only_used_s1_names(self):
        raw = (ROOT / generator.SCHEMA).read_bytes()
        schema = generator.load_schema(raw)
        self.assertEqual(len(schema['objects']), 34)
        self.assertEqual(sum(len(o['fields']) for o in schema['objects'].values()), 357)
        migrated = {name for name, obj in schema['objects'].items() if obj.get('generated')}
        self.assertEqual(migrated, set(schema['objects']))
        outputs = generator.render(schema, raw, Path(generator.__file__).read_bytes())
        definitions = generator.generated_values(outputs[generator.CPP].decode())
        self.assertEqual(len(definitions), 320)
        generator.audit_emitters(ROOT, schema, definitions)
        self.assertNotIn(b'::space_launch_mode {', outputs[generator.CPP])
        self.assertNotIn(b'LIVE_TUNING', outputs[generator.JS])
        self.assertEqual(set(re.findall(rb'export const (\w+)', outputs[generator.JS])),
                         {b'SPACES_CAPACITY', b'HOST_POWER', b'SESSION_ENCODER_SELECTION', b'LINUX_GPU_PROFILE',
                          b'PYROWAVE_BITRATE'})
        for reader in schema['objects']['spaces_capacity']['consumers']['polaris_web']:
            source = (ROOT / reader['file']).read_text()
            self.assertIn('import { SPACES_CAPACITY }', source)
            self.assertIn('SPACES_CAPACITY.CONCURRENT_ACTIVE', source)
            self.assertIn('SPACES_CAPACITY.CONCURRENT_LIMIT', source)
            self.assertNotIn('.capacity.concurrent_', source)

    def test_names_from_reuses_only_shared_fields_and_keeps_extra_names(self):
        self.schema['objects']['borrower'] = {
            'fields': ['value', 'extra'], 'generated': True, 'names_from': 'sample',
            # Names are shared even where the existing endpoints use different value types.
            'types': {'value': 'string', 'extra': 'boolean'},
            'polaris_emitter': {'file': 'borrower.cpp', 'function': 'borrow',
                'style': 'initializer_list', 'declaration': 'return {'}}
        self.write('borrower.cpp', 'json borrow() { return {{v1::sample::value, "legacy"}, {v1::borrower::extra, true}}; }')
        outputs = generator.render(self.schema, b'schema', b'generator')
        definitions = generator.generated_values(outputs[generator.CPP].decode())
        self.assertEqual(definitions, {'v1::sample::value': 'value', 'v1::borrower::extra': 'extra'})
        self.assertEqual(generator.owner_name(self.schema, 'borrower', 'value'), 'sample')
        self.assertEqual(generator.owner_name(self.schema, 'borrower', 'extra'), 'borrower')
        generator.audit_emitters(self.root, self.schema, definitions)

    def test_names_from_refuses_unknown_owners_cycles_and_wrong_field_ownership(self):
        for owner in ('missing', 'sample'):
            with self.subTest(owner=owner):
                changed = copy.deepcopy(self.schema)
                changed['objects']['sample']['names_from'] = owner
                with self.assertRaises(ValueError):
                    generator.validate(changed)
        changed = copy.deepcopy(self.schema)
        changed['objects']['borrower'] = {**copy.deepcopy(changed['objects']['sample']), 'names_from': 'sample'}
        changed['objects']['sample']['names_from'] = 'borrower'
        with self.assertRaises(ValueError):
            generator.validate(changed)
        with self.assertRaisesRegex(ValueError, 'does not serve field'):
            generator.owner_name(self.schema, 'sample', 'not_served')
        # An unrelated namespace with the same wire value must not pass ownership by coincidence.
        self.write('sample.cpp', 'json emit() { return {{v1::other::value, 7}}; }')
        with self.assertRaisesRegex(ValueError, 'incorrect generated field owner'):
            generator.audit_emitters(self.root, self.schema, {'v1::other::value': 'value'})

    def test_each_emitter_is_independently_inspected_not_generated_output(self):
        self.schema['objects']['sample']['polaris_emitter'] = [
            self.schema['objects']['sample']['polaris_emitter'],
            {'file': 'second.cpp', 'function': 'second', 'style': 'initializer_list', 'declaration': 'out["child"] = {'}]
        self.write('second.cpp', 'json second() { out["child"] = {{v1::sample::value, 8}}; out["later"] = {{"unrelated",9}}; }')
        self.store_schema()
        self.assertEqual(self.invoke()[0], 0)
        self.write('second.cpp', 'json second() { out["child"] = {{"value",8}}; }')
        code, output = self.invoke('--check')
        self.assertEqual(code, 1)
        self.assertIn('literal keys', output)
        self.write('second.cpp', 'json second() { out["child"] = {{v1::sample::missing,8}}; }')
        self.assertIn('undefined generated key', self.invoke('--check')[1])

    def test_literal_and_generated_forms_resolve_only_actual_key_positions(self):
        definitions = {'v1::sample::value': 'value'}
        source = 'json emit() { /* {"not_a_key", 0} */ return {{v1::sample::value, {"value"}}}; }'
        emitter = self.schema['objects']['sample']['polaris_emitter']
        self.assertEqual(generator.emitter_fields(source, emitter, definitions), ({'value'}, set()))
        source = 'json emit() { return {{"value", 7}}; }'
        self.assertEqual(generator.emitter_fields(source, emitter, definitions), ({'value'}, {'value'}))
        subscript = {'function': 'emit', 'style': 'subscript', 'variable': 'out'}
        source = 'json emit() { out[v1::sample::value] = 7; auto read = out["not_written"]; }'
        self.assertEqual(generator.emitter_fields(source, subscript, definitions), ({'value'}, set()))

    def test_nullable_presence_is_independent_of_value_and_defaults_are_literal_pins(self):
        schema = generator.load_schema((ROOT / generator.SCHEMA).read_bytes())
        units = schema['objects']['bitrate_units']
        self.assertEqual(set(units['nullable']), {'cap_kbps', 'cap_source', 'split_kbps'})
        self.assertTrue(set(units['nullable']) <= set(units['required_present']))
        omitted = schema['objects']['space_launch_mode']['omitted']
        self.assertEqual(omitted['follows_host_default']['reader_defaults'],
                         {'nova_android': True, 'nova_shared': True, 'nova_deck': True})
        self.assertIsNone(omitted['launch_as_available']['reader_defaults']['nova_android'])
        self.assertEqual(omitted['launch_as']['reader_defaults']['nova_deck'],
                         {'launch_as_present': False, 'launch_as': ''})
        self.assertEqual(omitted['launch_as_unavailable_reason']['reader_defaults']['nova_shared'], '')
        changed = copy.deepcopy(schema)
        changed['objects']['bitrate_units']['required_present'].remove('split_kbps')
        with self.assertRaisesRegex(ValueError, 'nullable generated fields'):
            generator.validate(changed)

    def test_subset_omissions_require_exact_parent_difference_and_explicit_defaults(self):
        schema = generator.load_schema((ROOT / generator.SCHEMA).read_bytes())
        generator.validate(schema)
        changed = copy.deepcopy(schema)
        del changed['objects']['space_launch_mode']['omitted']['launch_as']
        with self.assertRaisesRegex(ValueError, 'parent minus subset'):
            generator.validate(changed)
        changed = copy.deepcopy(schema)
        del changed['objects']['space_launch_mode']['omitted']['launch_as']['reader_defaults']['nova_android']
        with self.assertRaisesRegex(ValueError, 'every reader default'):
            generator.validate(changed)


    def test_stale_drift_uses_actual_reads_and_does_not_suppress_omissions(self):
        self.assertEqual(generator.stale_drift({'value'}, {'value'}, [], ['value']), {'value'})
        self.assertEqual(generator.stale_drift({'value'}, set(), [], ['value']), set())
        self.assertEqual(generator.stale_drift({'value'}, {'extra'}, ['extra'], []), set())
        self.assertEqual(generator.stale_drift({'value'}, {'extra'}, ['extra'], [], omitted={'extra'}), {'extra'})

    def test_digit_separators_before_an_emitter_do_not_consume_its_scope(self):
        source = """auto prior = 2'000'000'000;
json target() { return {{v1::sample::value, 1}}; }
char later = 'q';
"""
        emitter = {'function': 'target', 'style': 'initializer_list', 'declaration': 'return {'}
        self.assertEqual(generator.emitter_fields(source, emitter, {'v1::sample::value': 'value'}),
                         ({'value'}, set()))

    def test_raw_string_contents_do_not_close_an_emitter_scope(self):
        source = '''json target() {
  auto text = u8R"tag(" } // raw contents are not syntax
)tag";
  return {{v1::sample::value, 1}};
}
'''
        emitter = {'function': 'target', 'style': 'initializer_list', 'declaration': 'return {'}
        self.assertEqual(generator.emitter_fields(source, emitter, {'v1::sample::value': 'value'}),
                         ({'value'}, set()))



if __name__ == '__main__':
    unittest.main(verbosity=2)
