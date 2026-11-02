"""Verify immutable per-version source/firmware manifests; never writes files."""
import argparse
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('version', nargs='?', default='V1.0')
a = ap.parse_args()
version = root / a.version
manifest = json.loads((version / 'SNAPSHOT.json').read_text(encoding='utf-8'))
expected = manifest['files']
errors = []
for name, wanted in expected.items():
    path = version / name
    if not path.is_file():
        errors.append('MISSING '+name)
    elif hashlib.sha256(path.read_bytes()).hexdigest() != wanted:
        errors.append('CHANGED '+name)
ignore = {'SNAPSHOT.json'}
extra = [p.relative_to(version).as_posix() for p in version.rglob('*')
         if p.is_file() and p.relative_to(version).as_posix() not in expected
         and p.name not in ignore and '__pycache__' not in p.parts
         and not any(part.startswith('build') or part in ('captures', 'test_results')
                     for part in p.relative_to(version).parts[:-1])]
if extra:
    errors += ['EXTRA '+name for name in extra]
if errors:
    print('\n'.join(errors))
    raise SystemExit(1)
print('%s: %d files match SHA-256 snapshot' % (a.version, len(expected)))
