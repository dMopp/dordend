#!/usr/bin/env python3
"""Prepare stable release assets from the five tested artifacts of one CI run."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile


def only(directory, pattern):
    matches = list(directory.glob(pattern))
    if len(matches) != 1 or not matches[0].is_file():
        raise ValueError(f'Expected exactly one {pattern} in {directory}')
    return matches[0]


def debian_source_files(directory, identity):
    """Check the unsigned 3.0 (quilt) descriptor before publishing its components."""
    upstream = f"{identity['version']}+git{identity['timestamp']}.git{identity['commit'][:12]}"
    version = upstream + '-1'
    descriptor = only(directory, 'rummage_*.dsc')
    fields = {}
    current = None
    for line in descriptor.read_text().splitlines():
        if line.startswith((' ', '\t')) and current:
            fields[current] += '\n' + line.strip()
        elif ':' in line:
            current, value = line.split(':', 1)
            if current in fields:
                raise ValueError('Duplicate Debian source descriptor field')
            fields[current] = value.strip()
        else:
            current = None
    if (fields.get('Source'), fields.get('Version'), fields.get('Format')) != ('rummage', version, '3.0 (quilt)'):
        raise ValueError('Debian source descriptor does not match the snapshot')
    expected = {f'rummage_{upstream}.orig.tar.gz', f'rummage_{version}.debian.tar.xz'}
    found = set()
    components = []
    for line in fields.get('Checksums-Sha256', '').splitlines():
        if not line.strip():
            continue
        digest, size, name = line.split()
        if name not in expected or name in found:
            raise ValueError('Unexpected Debian source component')
        path = directory / name
        if path.stat().st_size != int(size) or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('Debian source component checksum mismatch')
        if name.endswith('.orig.tar.gz') and digest != identity['sha256']:
            raise ValueError('Debian original source differs from the checkout archive')
        found.add(name)
        components.append(path)
    if found != expected:
        raise ValueError('Missing Debian source component')
    return [descriptor, *components,
            only(directory, 'rummage_*_source.changes'),
            only(directory, 'rummage_*_source.buildinfo')]


def prepare(artifacts, output, commit, repository, run_id, readme):
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Expected a full Git commit SHA')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository) or not run_id.isdigit():
        raise ValueError('Invalid repository or workflow run ID')
    if output.exists() and any(output.iterdir()):
        raise ValueError('Output directory must be empty')
    roots = {
        'void': artifacts / f'void-x86_64-{commit}' / 'void-package',
        'deb': artifacts / f'deb-x86_64-{commit}' / 'native-package',
        'rpm': artifacts / f'rpm-x86_64-{commit}' / 'native-package',
        'arch': artifacts / f'arch-x86_64-{commit}' / 'native-package',
        'tumbleweed': artifacts / f'tumbleweed-x86_64-{commit}' / 'native-package',
    }
    identities = [json.loads((root / ('source.json' if kind == 'void' else 'input/source.json')).read_text())
                  for kind, root in roots.items()]
    identity = identities[0]
    if any(item != identity for item in identities) or identity['commit'] != commit:
        raise ValueError('Package artifacts do not all match the expected source revision')
    sources = [roots[kind] / 'input/source.tar.gz' for kind in ('deb', 'rpm', 'arch', 'tumbleweed')]
    if any(hashlib.sha256(source.read_bytes()).hexdigest() != identity['sha256'] for source in sources):
        raise ValueError('Source archive checksum mismatch')
    debian_sources = debian_source_files(roots['deb'] / 'debian-source', identity)
    deb = only(roots['deb'] / 'packages', 'rummage_*_amd64.deb')
    rpm = only(roots['rpm'] / 'packages', 'rummage-[0-9]*.x86_64.rpm')
    arch = only(roots['arch'] / 'packages', 'rummage-[0-9]*-x86_64.pkg.tar.zst')
    tumbleweed = only(roots['tumbleweed'] / 'packages', 'rummage-[0-9]*.x86_64.rpm')
    void = only(roots['void'] / 'packages', 'rummage-[0-9]*.x86_64.xbps')
    media_void = only(roots['void'] / 'packages', 'rummage-media-*.x86_64.xbps')
    media_assets = {
        'rummage-media-debian13-amd64.deb': only(roots['deb'] / 'packages', 'rummage-media_*_amd64.deb'),
        'rummage-media-fedora44-x86_64.rpm': only(roots['rpm'] / 'packages', 'rummage-media-[0-9]*.x86_64.rpm'),
        'rummage-media-arch-x86_64.pkg.tar.zst': only(roots['arch'] / 'packages', 'rummage-media-[0-9]*-x86_64.pkg.tar.zst'),
        'rummage-media-tumbleweed-x86_64.rpm': only(roots['tumbleweed'] / 'packages', 'rummage-media-[0-9]*.x86_64.rpm'),
    }
    index = roots['void'] / 'packages/x86_64-repodata'
    if not index.is_file() or index.stat().st_size == 0:
        raise ValueError('Void repository index is missing')
    instructions = readme.read_text().split('<!-- development-downloads:start -->', 1)[1].split(
        '<!-- development-downloads:end -->', 1)[0].strip()
    output.mkdir(parents=True, exist_ok=True)
    for name, source in media_assets.items():
        shutil.copyfile(source, output / name)
    shutil.copyfile(deb, output / 'rummage-debian13-amd64.deb')
    shutil.copyfile(rpm, output / 'rummage-fedora44-x86_64.rpm')
    shutil.copyfile(arch, output / 'rummage-arch-x86_64.pkg.tar.zst')
    shutil.copyfile(tumbleweed, output / 'rummage-tumbleweed-x86_64.rpm')
    with tarfile.open(output / 'rummage-void-glibc-x86_64.tar.gz', 'w:gz') as archive:
        for source in (void, media_void, index):
            archive.add(source, arcname=f'rummage-void-x86_64/packages/{source.name}')
        archive.add(roots['void'] / 'source.json', arcname='rummage-void-x86_64/source.json')
    with tarfile.open(output / 'rummage-debian-source.tar.gz', 'w:gz') as archive:
        for source in debian_sources:
            archive.add(source, arcname=f'rummage-debian-source/{source.name}')
    shutil.copyfile(sources[0], output / 'rummage-source.tar.gz')
    (output / 'source.json').write_text(json.dumps(identity, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(
        f'{hashlib.sha256(asset.read_bytes()).hexdigest()}  {asset.name}\n'
        for asset in sorted(output.iterdir())))
    (output / 'release-notes.md').write_text(
        f"Built from [`{commit[:12]}`](https://github.com/{repository}/commit/{commit}). "
        f"Application version: {identity['version']}.\n\n"
        f"[Successful build and checks](https://github.com/{repository}/actions/runs/{run_id}). "
        'The `development` tag and these assets move together as newer main builds pass CI. '
        'Save `source.json` to retain the exact source identity.\n\n' + instructions + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifacts', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--readme', type=Path, default=Path('README.md'))
    args = parser.parse_args()
    prepare(args.artifacts, args.output, args.commit, args.repository, args.run_id, args.readme)
