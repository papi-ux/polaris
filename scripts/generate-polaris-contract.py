#!/usr/bin/env python3
"""Generate committed S1 Polaris wire names; stdlib only, no build-time generation.

The schema's ordered fields remain compatible with format-1 readers. Only
generated:true objects produce names; subsets reuse their parent's namespace.
Nova outputs/vendoring are a separate S1b change, never an automatic update.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import sys


SCHEMA = 'docs/nova-contract.json'
CPP = 'src/generated/polaris_v1_contract.h'
JS = 'src_assets/common/assets/web/generated/polarisV1Contract.js'
IDENTIFIER = re.compile(r'[a-z][a-z0-9_]*\Z')
CPP_KEYWORDS = set(('alignas alignof and and_eq asm auto bitand bitor bool break case catch char '
    'char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast '
    'continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum '
    'explicit export extern false float for friend goto if inline int long mutable namespace new '
    'noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast '
    'requires return short signed sizeof static static_assert static_cast struct switch template '
    'this thread_local throw true try typedef typeid typename union unsigned using virtual void '
    'volatile wchar_t while xor xor_eq').split())
TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|'
                   r'(?:u8|u|U|L)?R"(?P<raw_delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=raw_delimiter)"|'
                   r"[0-9][0-9A-Za-z_]*(?:'[0-9A-Za-z_]+)*|"
                   r'"(?:\\.|[^"\\])*"|'
                   r"'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|::|==|!=|[^\s]")


def unique_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError('duplicate schema member: ' + name)
        result[name] = value
    return result


def load_schema(raw: bytes) -> dict:
    return json.loads(raw, object_pairs_hook=unique_object)


def consumers(obj: dict):
    for kind, regions in obj.get('consumers', {}).items():
        for region in regions if isinstance(regions, list) else [regions]:
            yield kind, region


def owner_name(schema: dict, name: str, field: str | None = None) -> str:
    """Canonical namespace for a field; borrowers retain their extra fields."""
    seen = set()
    while True:
        if name not in schema['objects']:
            raise ValueError('unknown names owner: ' + name)
        if name in seen:
            raise ValueError('names ownership cycle: ' + name)
        seen.add(name)
        obj = schema['objects'][name]
        if field is not None and field not in obj['fields']:
            raise ValueError('owner does not serve field: ' + name + '.' + field)
        parent = obj.get('subset_of', obj.get('names_from'))
        if parent is None:
            return name
        if parent not in schema['objects']:
            raise ValueError('unknown names owner: ' + parent)
        if 'names_from' in obj and field is not None and field not in schema['objects'][parent]['fields']:
            return name
        name = parent


def validate(schema: dict) -> None:
    if schema.get('version') != 2 or not isinstance(schema.get('objects'), dict):
        raise ValueError('generator requires additive schema format 2')
    for name, obj in schema['objects'].items():
        if not IDENTIFIER.fullmatch(name) or name in CPP_KEYWORDS:
            raise ValueError('invalid/reserved object name: ' + name)
        fields = obj['fields']
        if len(set(fields)) != len(fields):
            raise ValueError('constant collision in ' + name)
        for field in fields:
            if not IDENTIFIER.fullmatch(field) or field in CPP_KEYWORDS:
                raise ValueError('invalid/reserved field name: ' + name + '.' + field)
        for key in ('optional', 'nullable', 'required_present'):
            if not set(obj.get(key, [])) <= set(fields):
                raise ValueError(key + ' names an unknown field: ' + name)
        if set(obj.get('optional', [])) & set(obj.get('required_present', [])):
            raise ValueError('optional field is required-present: ' + name)
        if obj.get('generated'):
            if set(obj.get('types', {})) != set(fields):
                raise ValueError('generated types must describe every field: ' + name)
            if not set(obj.get('nullable', [])) <= set(obj.get('required_present', [])):
                raise ValueError('nullable generated fields must declare presence: ' + name)
        omitted = obj.get('omitted', {})
        if 'names_from' in obj:
            if 'subset_of' in obj:
                raise ValueError('names_from and subset_of are exclusive: ' + name)
            owner_name(schema, name)
            parent = schema['objects'][obj['names_from']]
            shared = set(fields) & set(parent['fields'])
            if not shared:
                raise ValueError('names_from must share fields: ' + name)
            if obj.get('generated') and not parent.get('generated'):
                raise ValueError('names owner must be generated: ' + name)
        if 'subset_of' in obj:
            parent = schema['objects'].get(obj['subset_of'])
            if parent is None or not set(fields) <= set(parent['fields']):
                raise ValueError('invalid subset: ' + name)
            owner_name(schema, name)
            if set(omitted) != set(parent['fields']) - set(fields):
                raise ValueError('omitted keys must equal parent minus subset: ' + name)
            kinds = {kind for kind, _ in consumers(obj)}
            for field, declaration in omitted.items():
                if not declaration.get('reason') or set(declaration.get('reader_defaults', {})) != kinds:
                    raise ValueError('omission needs reason and every reader default: ' + name + '.' + field)
        elif omitted:
            raise ValueError('omitted requires subset_of: ' + name)
        known = set(schema.get('known_drift', {}).get('nova_reads_polaris_never_sends', {}).get(name, []))
        for kind, reader in consumers(obj):
            reads = reader.get('reads', [])
            if not reader.get('file') or len(set(reads)) != len(reads):
                raise ValueError('invalid consumer declaration: ' + name + '/' + kind)
            if not set(reads) <= set(fields) | set(omitted) | known:
                raise ValueError('consumer reads undeclared field: ' + name + '/' + kind)


def tokens(source: str) -> list[str]:
    return [m[0] for m in TOKEN.finditer(source) if not m[0].startswith(('//', '/*'))]


def balanced(parts: list[str], at: int, left='{', right='}') -> list[str]:
    depth = 0
    for end in range(at, len(parts)):
        if parts[end] == left:
            depth += 1
        elif parts[end] == right:
            depth -= 1
            if depth == 0:
                return parts[at:end + 1]
    raise ValueError('unclosed source scope')


def cpp_scope(source: str, function: str | None) -> list[str]:
    parts = tokens(source)
    if not function:
        return parts
    for at, token in enumerate(parts[:-1]):
        if token != function or parts[at + 1] != '(':
            continue
        arguments = balanced(parts, at + 1, '(', ')')
        start = at + 1 + len(arguments)
        if start < len(parts) and parts[start] == '{':
            return balanced(parts, start)
    raise ValueError('emitter function not found: ' + function)


def generated_values(header: str) -> dict[str, str]:
    values = {}
    for group in re.finditer(r'namespace polaris::contract::v1::(\w+)\s*\{([^}]+)\}', header):
        for field, value in re.findall(r'inline constexpr char (\w+)\[\] = "(\w+)";', group[2]):
            token = 'v1::' + group[1] + '::' + field
            if token in values:
                raise ValueError('duplicate generated definition: ' + token)
            values[token] = value
    return values


def key_token(parts: list[str], at: int, definitions: dict[str, str]):
    if parts[at].startswith('"'):
        return json.loads(parts[at]), at + 1, False
    if parts[at:at + 2] == ['v1', '::'] and at + 4 < len(parts):
        token = ''.join(parts[at:at + 5])
        if token not in definitions:
            raise ValueError('undefined generated key: ' + token)
        return definitions[token], at + 5, True
    return None, at + 1, False


def emitter_fields(source: str, emitter: dict, definitions: dict[str, str], owners: dict[str, str] | None = None):
    parts = cpp_scope(source, emitter.get('function'))
    if emitter['style'] == 'initializer_list':
        declaration = tokens(emitter['declaration'])
        at = next((i for i in range(len(parts)) if parts[i:i + len(declaration)] == declaration), None)
        if at is None:
            raise ValueError('initializer declaration not found: ' + emitter['declaration'])
        parts = balanced(parts, at + len(declaration) - 1)
    found, literals = set(), set()
    for at in range(len(parts) - 1):
        if emitter['style'] == 'initializer_list':
            if parts[at] != '{':
                continue
            key_at = at + 1
            key, end, generated = key_token(parts, key_at, definitions)
            is_key = end < len(parts) and parts[end] == ','
        else:
            if parts[at:at + 2] != [emitter['variable'], '[']:
                continue
            key_at = at + 2
            key, end, generated = key_token(parts, key_at, definitions)
            is_key = parts[end:end + 2] == [']', '=']
        if key is not None and is_key:
            if generated and owners is not None and ''.join(parts[key_at:end]) != owners.get(key):
                raise ValueError('incorrect generated field owner: ' + ''.join(parts[key_at:end]))
            found.add(key)
            if not generated:
                literals.add(key)
    return found, literals


def audit_emitters(root: Path, schema: dict, definitions: dict[str, str]) -> None:
    """Derive actual keys at each migrated source site, never from its generated list."""
    for name, obj in schema['objects'].items():
        if not obj.get('generated'):
            continue
        emitters = obj['polaris_emitter']
        owners = {field: 'v1::' + owner_name(schema, name, field) + '::' + field for field in obj['fields']}
        for emitter in emitters if isinstance(emitters, list) else [emitters]:
            actual, literals = emitter_fields((root / emitter['file']).read_text(), emitter, definitions, owners)
            expected = set(emitter.get('fields', obj['fields']))
            if actual != expected or literals:
                raise ValueError(f'{name}: actual emitter keys {sorted(actual)} / literal keys {sorted(literals)} '
                                 f'differ from {sorted(expected)} at {emitter["file"]}')


def stale_drift(served, read, known_missing, known_unread, omitted=(), informational=()):
    """A recorded finding is live only if actual scoped reads still demonstrate it."""
    actual_missing = set(read) - set(served) - set(omitted)
    actual_unread = set(served) - set(read) - set(informational)
    return (set(known_missing) - actual_missing) | (set(known_unread) - actual_unread)


def render(schema: dict, schema_bytes: bytes, generator_bytes: bytes) -> dict[str, bytes]:
    validate(schema)
    provenance = ('// Generated by scripts/generate-polaris-contract.py; do not edit.\n'
                  '// schema-sha256: ' + hashlib.sha256(schema_bytes).hexdigest() + '\n'
                  '// generator-sha256: ' + hashlib.sha256(generator_bytes).hexdigest() + '\n')
    cpp = [provenance, '#pragma once\n\n']
    js = [provenance]
    exports = set()
    for name, obj in schema['objects'].items():
        if not obj.get('generated'):
            continue
        owned = [field for field in obj['fields'] if owner_name(schema, name, field) == name]
        if owned:
            cpp.append('namespace polaris::contract::v1::' + name + ' {\n')
            for field in owned:
                cpp.append(f'  inline constexpr char {field}[] = "{field}";\n')
            cpp.append('}\n\n')
        reads = {field for kind, reader in consumers(obj) if kind == 'polaris_web' for field in reader['reads']}
        if reads:
            export = name.upper()
            if export in exports:
                raise ValueError('JS constant collision: ' + export)
            exports.add(export)
            js.append('export const ' + export + ' = Object.freeze({\n')
            for field in obj['fields']:
                if field in reads:
                    js.append(f"  {field.upper()}: '{field}',\n")
            js.append('})\n')
    return {CPP: ''.join(cpp).encode(), JS: ''.join(js).encode()}


def write_or_check(root: Path, outputs: dict[str, bytes], check: bool) -> bool:
    clean = True
    for name, expected in outputs.items():
        path = root / name
        if check:
            actual = path.read_bytes() if path.is_file() else b''
            if actual != expected:
                clean = False
                print('stale generated output: ' + name, file=sys.stderr)
                sys.stderr.writelines(difflib.unified_diff(actual.decode(errors='replace').splitlines(True),
                    expected.decode().splitlines(True), fromfile=name, tofile=name + ' (expected)'))
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(expected)
    return clean


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args(argv)
    try:
        raw = (args.root / SCHEMA).read_bytes()
        schema = load_schema(raw)
        outputs = render(schema, raw, Path(__file__).read_bytes())
        # Resolve actual key tokens against definitions that --check byte-compares below.
        audit_emitters(args.root, schema, generated_values(outputs[CPP].decode()))
        if not write_or_check(args.root, outputs, args.check):
            return 1
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('contract generation refused: ' + str(error), file=sys.stderr)
        return 1
    print('S1 contract names checked' if args.check else 'S1 contract names generated')
    return 0


if __name__ == '__main__':
    sys.exit(main())
