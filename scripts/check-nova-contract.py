#!/usr/bin/env python3
"""Diff Polaris' served fields against what a Nova checkout actually reads.

Nova and Polaris ship from separate repositories, so neither build can see the
other. Both address the same JSON by string literal, and both sides fail soft:
Nova defaults a field it cannot find, Polaris never learns that something went
unread. The result is a feature that quietly stops working instead of breaking.

This paired developer check needs both repositories. Format 2 reads the Nova
side from an explicit immutable Git revision. Polaris' own CI checks generated
outputs and tests/integration/test_nova_contract.cpp audits the producer.

    scripts/check-nova-contract.py --nova /path/to/nova --nova-revision EXACT_40_HEX

Exits non-zero for undeclared drift, stale known_drift, missing readers, or an
invalid migrated schema/emitter. Declared omissions retain their reader defaults;
this source check does not establish runtime or device acceptance.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import json
import re
import sys
import subprocess
from pathlib import Path

# Nova reads responses through org.json accessors. Anything it asks for by name
# is a field it expects Polaris to serve.
ACCESSORS = "optString|optBoolean|optInt|optLong|optDouble|optJSONObject|optJSONArray|opt|getString|getBoolean|getInt|getLong|getJSONObject|getJSONArray|has|isNull"


class ReaderUnavailableError(ValueError):
    """A declared consumer is absent in the inspected Nova revision."""


def function_body(source: str, name: str) -> str:
    """The body of one Kotlin function, up to the next declaration at its level.

    Scope has to be the function, and getting this wrong has already produced two
    rounds of false findings. A file-wide scan reports the nested launch_mode and
    mode fields as game fields. Scoping by receiver name is no better here,
    because PolarisGameJsonAdapter.kt gives four different functions a parameter
    called `json` — so artwork manifest and asset fields land in the game set and
    the tool reports drift that does not exist.
    """
    start = re.search(
        rf'^([ \t]*)(?:internal |private |public )?fun {re.escape(name)}\(',
        source,
        re.M,
    )
    if not start:
        raise ReaderUnavailableError(f"function not found: {name}")
    rest = source[start.end():]
    # Local helper functions are deeper-indented and remain part of the reader
    # scope. Only a sibling declaration at the exact same indentation closes
    # the method whose JSON reads we are auditing.
    indent = re.escape(start.group(1))
    nxt = re.search(
        rf'^{indent}(?:internal |private |public )?fun \w+\(',
        rest,
        re.M,
    )
    return rest[: nxt.start()] if nxt else rest


def reads_for_object(source: str, reader: dict) -> set[str]:
    """Keys read for one contract object."""
    body = function_body(source, reader["function"])
    # `?.` as well as `.`: Nova reaches nullable sub-objects with the safe-call
    # operator, and requiring a bare dot silently reports every one of their
    # fields as unread.
    pattern = re.compile(
        rf'\b{re.escape(reader["receiver"])}\??\.(?:{ACCESSORS})\(\s*"([a-zA-Z_0-9]+)"'
    )
    # PolarisGameJsonAdapter routes every double through its finiteDouble
    # helper, so optDouble never appears against the receiver and the accessor
    # scan above reports those fields as unread. The helper call still names
    # the receiver and the literal key, so count it as the read it is.
    helper = re.compile(
        rf'\bfiniteDouble\(\s*{re.escape(reader["receiver"])}\s*,\s*"([a-zA-Z_0-9]+)"'
    )
    strict_helper = re.compile(
        rf'\bstrict(?:String|Boolean)\(\s*{re.escape(reader["receiver"])}\s*,\s*"([a-zA-Z_0-9]+)"'
    )
    return set(pattern.findall(body)) | set(helper.findall(body)) | set(strict_helper.findall(body))


def polaris_manifest(repo: Path) -> dict:
    return json.loads((repo / "docs" / "nova-contract.json").read_text())


def nova_reads(nova: Path, reader: dict) -> set[str]:
    """Fields Nova reads for one contract object."""
    path = nova / reader["file"]
    if not path.is_file():
        raise ReaderUnavailableError(f"reader file not found: {reader['file']}")
    return reads_for_object(path.read_text(), reader)



def generator_module(polaris: Path):
    spec = importlib.util.spec_from_file_location('polaris_contract_generator', polaris / 'scripts/generate-polaris-contract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def source_region(source: str, reader: dict, generator) -> str:
    if 'scope_start' in reader:
        start = source.find(reader['scope_start'])
        if start < 0:
            raise ReaderUnavailableError('scope start not found: ' + reader['scope_start'])
        start += len(reader['scope_start'])
        end = source.find(reader['scope_end'], start)
        return source[start:end] if end >= 0 else source[start:]
    if reader.get('style', '').startswith('kotlin') or reader.get('style') is None:
        return function_body(source, reader['function'])
    if reader.get('style') == 'javascript':
        start = source.find('function ' + reader['function'] + '(')
        if start < 0:
            raise ReaderUnavailableError('JS function not found: ' + reader['function'])
        end = source.find('\nexport function ', start + 1)
        return source[start:end] if end >= 0 else source[start:]
    return ' '.join(generator.cpp_scope(source, reader['function'])).replace(' :: ', '::')


def consumer_reads(source: str, reader: dict, definitions: dict[str, str], generator) -> set[str]:
    style = reader.get('style', 'kotlin_org_json')
    body = source_region(source, reader, generator)
    if style != 'qt_json':
        body = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/', '', body)
    literal = r'"[a-zA-Z_0-9]+"'
    generated = r'(?:PolarisV1\.\w+\.\w+|v1::\w+::\w+|[A-Z_]+\.[A-Z_]+)'
    argument = '(' + literal + '|' + generated + ')'
    if style == 'kotlin_serial_name':
        candidates = re.findall(r'@SerialName\(\s*' + argument, body)
    elif style == 'qt_json':
        candidates = re.findall(r'\b' + re.escape(reader['receiver']) +
            r'\s*\.\s*(?:value|contains)\s*\(\s*(?:QStringLiteral\s*\(\s*)?' + argument, body)
    elif style == 'javascript':
        # The validation array drives receiver[key] reads in validSnapshot;
        # the other two regions use receiver[SPACES_CAPACITY.FIELD] directly.
        candidates = re.findall(r'\b[A-Z_]+\.[A-Z_]+\b', body)
        if not re.search(re.escape(reader['receiver']) + r'\s*\[', body):
            raise ReaderUnavailableError('declared JS receiver has no bracket read: ' + reader['receiver'])
    else:
        receiver = re.escape(reader['receiver'])
        candidates = re.findall(r'\b' + receiver + r'\??\.(?:' + ACCESSORS + r')\(\s*' + argument, body)
        candidates += re.findall(r'\b(?:finiteDouble|strictString|strictBoolean)\(\s*' + receiver + r'\s*,\s*' + argument, body)
        if reader.get('literal_key_list_helper'):
            candidates += re.findall(r'\b' + re.escape(reader['literal_key_list_helper']) + r'\(\s*' + argument, body)
    result = set()
    for token in candidates:
        if token.startswith('"'):
            result.add(json.loads(token))
        elif token in definitions:
            result.add(definitions[token])
        else:
            raise ReaderUnavailableError('undefined generated reader key: ' + token)
    return result


def javascript_definitions(source: str) -> dict[str, str]:
    result = {}
    for owner, body in re.findall(r'export const (\w+) = Object.freeze\(\{([^}]+)\}\)', source):
        for field, value in re.findall(r"(\w+): '([a-z0-9_]+)'", body):
            result[owner + '.' + field] = value
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--nova', required=True, type=Path, help='path to a Nova source repository')
    parser.add_argument('--nova-revision', help='exact 40-hex immutable Nova input; required by format 2')
    parser.add_argument('--polaris', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    if not args.nova.is_dir():
        parser.error('--nova must name an existing Nova source directory')
    manifest = polaris_manifest(args.polaris)
    format_two = manifest.get('version', 1) == 2
    if (format_two and not args.nova_revision) or (args.nova_revision and not re.fullmatch('[0-9a-f]{40}', args.nova_revision)):
        parser.error('--nova-revision must pin the exact 40-hex Nova source for format 2')
    generator = generator_module(args.polaris) if format_two else None
    new_findings = 0
    if format_two:
        try:
            generator.validate(manifest)
            definitions = generator.generated_values((args.polaris / generator.CPP).read_text())
            generator.audit_emitters(args.polaris, manifest, definitions)
        except (OSError, ValueError, KeyError, TypeError) as error:
            print('DRIFT  actual migrated emitter/schema check: ' + str(error))
            new_findings += 1
    drift = manifest.get('known_drift', {})
    known_missing = drift.get('nova_reads_polaris_never_sends', {})
    known_unread = drift.get('polaris_sends_nova_never_reads', {})
    sources = {}

    def source(file: str, host=False) -> str:
        key = (host, file)
        if key not in sources:
            try:
                if host or not args.nova_revision:
                    path = (args.polaris if host else args.nova) / file
                    if not path.is_file():
                        raise ReaderUnavailableError('reader file not found: ' + file)
                    sources[key] = path.read_text()
                else:
                    result = subprocess.run(['git', '-c', 'core.commitGraph=false', '--no-replace-objects',
                        '-C', str(args.nova), 'show', args.nova_revision + ':' + file], check=True,
                        capture_output=True, text=True, env={**os.environ, 'GIT_NO_LAZY_FETCH': '1'})
                    sources[key] = result.stdout
            except (OSError, subprocess.CalledProcessError) as error:
                raise ReaderUnavailableError('pinned reader unavailable: ' + file + ': ' + str(error)) from error
        return sources[key]

    def definitions_for(kind: str) -> dict[str, str]:
        if kind == 'polaris_web':
            return javascript_definitions(source(generator.JS, host=True))
        # Future S1b reader constants must resolve to real vendored definitions;
        # their use is not inferred from a name or from a Polaris-generated list.
        if kind == 'nova_deck':
            file = 'clients/deck/src/generated/polaris_v1_contract.h'
            try:
                return generator.generated_values(source(file))
            except ReaderUnavailableError:
                return {}
        file = 'shared/polaris/model/src/commonMain/kotlin/com/papi/nova/shared/polaris/contract/PolarisV1Contract.kt'
        try:
            raw = source(file)
        except ReaderUnavailableError:
            return {}
        result = {}
        for owner, body in re.findall(r'object (\w+)\s*\{([^{}]+)\}', raw):
            for field, value in re.findall(r'const val (\w+) = "(\w+)"', body):
                result['PolarisV1.' + owner + '.' + field] = value
        return result

    for name, obj in manifest['objects'].items():
        declared = list(generator.consumers(obj)) if format_two else []
        if not declared and obj.get('nova_reader'):
            declared = [('legacy', obj['nova_reader'])]
        served = set(obj['fields'])
        if not declared:
            print(f'{name}: Polaris serves {len(served)}, no Nova reader yet [not yet consumed]')
            continue
        union, available = set(), True
        for kind, reader in declared:
            label = name if kind == 'legacy' else name + '/' + kind
            try:
                raw = source(reader['file'], host=kind == 'polaris_web')
                read = reads_for_object(raw, reader) if kind == 'legacy' else consumer_reads(raw, reader, definitions_for(kind), generator)
                if kind != 'legacy' and read != set(reader['reads']):
                    raise ReaderUnavailableError('declared reads differ from actual scoped reads: ' + str(sorted(read)))
            except (ReaderUnavailableError, ValueError) as error:
                print(f'{label}: Polaris serves {len(served)}, Nova reader unavailable [drift]')
                print(f'    DRIFT  {error}')
                new_findings += 1
                available = False
                continue
            union.update(read)
            omitted = set(obj.get('omitted', {}))
            missing = read - served - omitted
            unread = served - read - set(obj.get('informational_ok', []))
            fresh_missing = sorted(missing - set(known_missing.get(name, [])))
            fresh_unread = sorted(unread - set(known_unread.get(name, [])))
            status = 'drift' if fresh_missing or fresh_unread else 'ok'
            print(f'{label}: Polaris serves {len(served)}, Nova reads {len(read)} [{status}]')
            for field in fresh_missing:
                print(f'    DRIFT  Nova reads [{field}] that Polaris never serves')
            for field in fresh_unread:
                print(f'    DRIFT  Polaris serves [{field}] that Nova never reads')
            outstanding = len(missing & set(known_missing.get(name, []))) + len(unread & set(known_unread.get(name, [])))
            if outstanding:
                print(f'    {outstanding} known drift field(s) still outstanding')
            new_findings += len(fresh_missing) + len(fresh_unread)
        if obj.get('omitted'):
            print(f'{name}: {len(obj["omitted"])} declared omissions; exact reader defaults retained (runtime tests separate)')
        if obj.get('nullable'):
            print(f'{name}: {len(obj["nullable"])} nullable fields; {len(obj.get("required_present", []))} required-present fields')
        if available:
            actual_missing = union - served - set(obj.get('omitted', {}))
            actual_unread = served - union - set(obj.get('informational_ok', []))
            stale = generator.stale_drift(served, union, known_missing.get(name, []),
                known_unread.get(name, []), obj.get('omitted', {}), obj.get('informational_ok', [])) if format_two else (
                (set(known_missing.get(name, [])) - actual_missing) | (set(known_unread.get(name, [])) - actual_unread))
            for field in sorted(stale):
                print(f'    DRIFT  stale known_drift entry: {name}.{field}')
            new_findings += len(stale)
    print()
    print('no new drift' if not new_findings else f'{new_findings} new drift finding(s)')
    return 1 if new_findings else 0


if __name__ == '__main__':
    sys.exit(main())
