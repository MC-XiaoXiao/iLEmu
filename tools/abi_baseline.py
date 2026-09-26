#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Compare ABI observations, not normative semantic expectations."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

def source_fields(repo):
    source = (repo / 'src/device_state/include/device_state/darwin_abi.hpp').read_text()
    source = re.sub('//[^'+chr(10)+']*', '', source)
    def members(name):
        body = source.split('struct ' + name + ' {', 1)[1].split(chr(10)+'};', 1)[0]
        declarations = re.findall('([a-zA-Z_][a-zA-Z_0-9:]*) +([a-z_][a-z_0-9]*) *[{;]', body.replace(chr(10), ' '))
        if len(declarations) != body.count(';'):
            raise ValueError('unrecognized declaration in ' + name)
        return declarations
    fields = []
    for kind, name in members('DarwinAbi'):
        if kind == 'DarwinGuestCapabilities':
            fields.extend(name + '.' + key for _, key in members(kind))
        else:
            fields.append(name)
    formatter = (repo / 'app/abi_command.cpp').read_text()
    reads = set(re.findall('abi[.]([a-z_][a-z_0-9]*(?:[.][a-z_][a-z_0-9]*)?)', formatter))
    if not fields or reads != set(fields):
        raise ValueError(f'exporter/source mismatch: missing={set(fields)-reads}, extra={reads-set(fields)}')
    return sorted(fields)

def capture(binary, repo):
    fields = source_fields(repo)
    result = subprocess.run([str(binary.resolve()), 'abi', '--all'], check=True, text=True, capture_output=True, timeout=30)
    lines = result.stdout.splitlines()
    if not lines or lines[0] != 'abi-catalog-schema: 1':
        raise ValueError('unsupported or missing catalog schema')
    profiles, current = {}, None
    for line in lines[1:]:
        if not line:
            continue
        key, sep, value = line.partition(': ')
        if not sep or not value:
            raise ValueError('invalid catalog line: ' + line)
        if key == 'abi':
            if value in profiles:
                raise ValueError('duplicate profile: ' + value)
            current = profiles[value] = {}
        else:
            if current is None or key in current:
                raise ValueError('missing profile or duplicate field: ' + line)
            current[key] = {'true': True, 'false': False}.get(value, value)
    paths = ['src/device_state/include/device_state/darwin_abi.hpp', 'src/device_state/darwin_kernel_configuration.cpp', 'app/abi_command.cpp']
    return {'schema': 1, 'kind': 'implementation-observation-not-normative', 'source_fields': fields, 'profiles': profiles,
            'provenance': {'head': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
                           'source_sha256': {p: hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in paths},
                           'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}}

def validate(data):
    if not isinstance(data, dict) or data.get('schema') != 1 or data.get('kind') != 'implementation-observation-not-normative':
        raise ValueError('unsupported baseline schema/kind')
    fields, profiles = data['source_fields'], data['profiles']
    if (not isinstance(fields, list) or not fields
            or any(not isinstance(field, str) or not field for field in fields)
            or len(fields) != len(set(fields))
            or not isinstance(profiles, dict) or not profiles):
        raise ValueError('empty/duplicate source fields or profiles')
    keys = None
    for name, config in profiles.items():
        if (not isinstance(name, str) or not name or not isinstance(config, dict)
                or len(config) != len(fields)+1 or 'os-release' not in config):
            raise ValueError('incomplete profile: ' + name)
        if keys is not None and keys != set(config):
            raise ValueError('inconsistent fields: ' + name)
        keys = set(config)
        if any(not isinstance(v, (str, bool)) or v in ('unknown', '') for v in config.values()):
            raise ValueError('invalid field value: ' + name)

def read_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate JSON key: ' + key)
            result[key] = value
        return result
    return json.loads(path.read_text(), object_pairs_hook=unique)

def compare(before, after):
    validate(before)
    validate(after)
    changes = []
    if before['source_fields'] != after['source_fields']:
        changes.append({'change': 'field-schema', 'before': before['source_fields'], 'after': after['source_fields']})
    old, new = before['profiles'], after['profiles']
    for name in sorted(old.keys() | new.keys()):
        if name not in old:
            changes.append({'change': 'profile-added', 'profile': name, 'after': new[name]})
        elif name not in new:
            changes.append({'change': 'profile-removed', 'profile': name, 'before': old[name]})
        else:
            for key in sorted(old[name].keys() | new[name].keys()):
                if old[name].get(key) != new[name].get(key):
                    changes.append({'change': 'old-profile-field', 'profile': name, 'field': key, 'before': old[name].get(key), 'after': new[name].get(key)})
    return changes

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    export = commands.add_parser('capture')
    export.add_argument('--binary', type=Path, required=True)
    export.add_argument('--repository', type=Path, default=Path(__file__).resolve().parents[1])
    export.add_argument('--output', type=Path, required=True)
    diff = commands.add_parser('compare')
    diff.add_argument('baseline', type=Path)
    diff.add_argument('candidate', type=Path)
    diff.add_argument('--allow-additions', action='store_true', help='allow only new profiles; never allow changes to existing profiles or field schema')
    args = parser.parse_args()
    if args.command == 'capture':
        snapshot = capture(args.binary, args.repository)
        validate(snapshot)
        args.output.write_text(json.dumps(snapshot, indent=2, ensure_ascii=False)+chr(10))
        print(f'Captured {len(snapshot["profiles"])} profiles, {len(snapshot["source_fields"])} ABI leaf fields')
        return 0
    changes = compare(read_json(args.baseline), read_json(args.candidate))
    print(json.dumps({'changes': changes}, indent=2, ensure_ascii=False))
    return int(any(c['change'] != 'profile-added' or not args.allow_additions for c in changes))

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        print('ABI baseline error: '+str(error), file=sys.stderr)
        sys.exit(2)
