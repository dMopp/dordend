import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('downloads', SCRIPTS / 'prepare-downloads.py')
downloads = importlib.util.module_from_spec(spec)
spec.loader.exec_module(downloads)
COMMIT = 'a' * 40


class DownloadsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.artifacts = self.root / 'artifacts'
        self.output = self.root / 'downloads'
        self.readme = self.root / 'README.md'
        self.readme.write_text('<!-- development-downloads:start -->\nInstall me.\n<!-- development-downloads:end -->')
        self.identity = dict(commit=COMMIT, version='0.5.0', timestamp='20261112000000',
                             sha256=hashlib.sha256(b'source').hexdigest())
        self.roots = {}
        for kind in ('void', 'deb', 'rpm', 'arch', 'tumbleweed'):
            root = self.artifacts / f'{kind}-x86_64-{COMMIT}' / ('void-package' if kind == 'void' else 'native-package')
            self.roots[kind] = root
            (root / 'packages').mkdir(parents=True)
            metadata = root / ('source.json' if kind == 'void' else 'input/source.json')
            metadata.parent.mkdir(exist_ok=True)
            metadata.write_text(json.dumps(self.identity))
            if kind != 'void':
                (root / 'input/source.tar.gz').write_bytes(b'source')
        for kind, names in {
            'void': ['nordend-0.5.0_1.x86_64.xbps', 'x86_64-repodata'],
            'arch': ['nordend-0.5.0-1-x86_64.pkg.tar.zst', 'nordend-debug-0.5.0-1-x86_64.pkg.tar.zst'],
            'tumbleweed': ['nordend-0.5.0-1.1.x86_64.rpm'],
            'deb': ['nordend_0.5.0_amd64.deb', 'nordend-dbgsym_0.5.0_amd64.deb'],
            'rpm': ['nordend-0.5.0.x86_64.rpm', 'nordend-debuginfo-0.5.0.x86_64.rpm', 'nordend-0.5.0.src.rpm'],
        }.items():
            for name in names:
                (self.roots[kind] / 'packages' / name).write_bytes(name.encode())

    def prepare(self):
        downloads.prepare(self.artifacts, self.output, COMMIT, 'owner/repo', '123', self.readme)

    def test_assets_and_instructions(self):
        self.prepare()
        self.assertEqual(len(list(self.output.iterdir())), 9)
        self.assertEqual((self.output / 'nordend-debian13-amd64.deb').read_bytes(), b'nordend_0.5.0_amd64.deb')
        self.assertEqual((self.output / 'nordend-fedora44-x86_64.rpm').read_bytes(), b'nordend-0.5.0.x86_64.rpm')
        self.assertEqual((self.output / 'nordend-arch-x86_64.pkg.tar.zst').read_bytes(), b'nordend-0.5.0-1-x86_64.pkg.tar.zst')
        self.assertEqual((self.output / 'nordend-tumbleweed-x86_64.rpm').read_bytes(), b'nordend-0.5.0-1.1.x86_64.rpm')
        with tarfile.open(self.output / 'nordend-void-glibc-x86_64.tar.gz') as archive:
            self.assertIn('nordend-void-x86_64/packages/x86_64-repodata', archive.getnames())
        for line in (self.output / 'SHA256SUMS').read_text().splitlines():
            digest, filename = line.split('  ')
            self.assertEqual(digest, hashlib.sha256((self.output / filename).read_bytes()).hexdigest())
        self.assertIn('Install me.', (self.output / 'release-notes.md').read_text())

    def test_mixed_source_rejected(self):
        self.identity['commit'] = 'b' * 40
        (self.roots['rpm'] / 'input/source.json').write_text(json.dumps(self.identity))
        with self.assertRaisesRegex(ValueError, 'source revision'):
            self.prepare()

    def test_tampered_archive_rejected(self):
        (self.roots['deb'] / 'input/source.tar.gz').write_bytes(b'wrong')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.prepare()

    def test_missing_void_index_rejected(self):
        (self.roots['void'] / 'packages/x86_64-repodata').unlink()
        with self.assertRaisesRegex(ValueError, 'index'):
            self.prepare()

    def test_ambiguous_package_rejected(self):
        (self.roots['deb'] / 'packages/nordend_0.6.0_amd64.deb').write_bytes(b'old')
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            self.prepare()

    def test_output_cannot_mix_old_assets(self):
        self.prepare()
        with self.assertRaisesRegex(ValueError, 'empty'):
            self.prepare()

    def publish(self, mode):
        self.prepare()
        binary = self.root / 'bin'
        binary.mkdir()
        mock = binary / 'gh'
        mock.write_text('''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
with open(os.environ['CALL_LOG'], 'a') as log:
    log.write(json.dumps(args)+'\\n')
if args[0] == 'api' and args[-1].endswith('/releases/tags/development'):
    mode = os.environ['MOCK_MODE']
    if mode in ('missing', 'error'):
        print('gh: Not Found (HTTP 404)' if mode == 'missing' else 'network unavailable', file=sys.stderr)
        sys.exit(1)
    print(json.dumps({'immutable': mode == 'immutable'}))
elif args[0] == 'api' and '/compare/' in args[1]:
    print('behind' if os.environ['MOCK_MODE'] == 'older' else 'ahead')
''')
        mock.chmod(0o755)
        log = self.root / 'calls'
        result = subprocess.run(['bash', str(SCRIPTS / 'publish-downloads.sh'), str(self.output), COMMIT],
                                env={**os.environ, 'PATH': str(binary) + ':' + os.environ['PATH'],
                                     'GH_REPO': 'owner/repo', 'MOCK_MODE': mode, 'CALL_LOG': str(log)},
                                capture_output=True, text=True)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        return result, calls

    def test_first_publication(self):
        result, calls = self.publish('missing')
        self.assertEqual(result.returncode, 0, result.stderr)
        create = next(c for c in calls if c[:2] == ['release', 'create'])
        self.assertIn('--prerelease', create)
        self.assertEqual(create[create.index('--target') + 1], COMMIT)

    def test_update_orders_checksums_last(self):
        result, calls = self.publish('existing')
        self.assertEqual(result.returncode, 0, result.stderr)
        uploads = [c for c in calls if c[:2] == ['release', 'upload']]
        self.assertEqual(len(uploads), 8)
        self.assertTrue(uploads[-1][3].endswith('/SHA256SUMS'))
        self.assertTrue(any('sha=' + COMMIT in c for c in calls))

    def test_api_failure_does_not_create_release(self):
        result, calls = self.publish('error')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(calls), 1)

    def test_immutable_release_fails_before_mutation(self):
        result, calls = self.publish('immutable')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(calls), 1)

    def test_old_rerun_does_not_roll_back_downloads(self):
        result, calls = self.publish('older')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any(c[0] == 'release' or '--method' in c for c in calls))

    def test_missing_arch_blocks_preparation(self):
        (self.roots['arch'] / 'packages/nordend-0.5.0-1-x86_64.pkg.tar.zst').unlink()
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            self.prepare()

    def test_tumbleweed_must_match_other_packages(self):
        self.identity['commit'] = 'b' * 40
        (self.roots['tumbleweed'] / 'input/source.json').write_text(json.dumps(self.identity))
        with self.assertRaisesRegex(ValueError, 'source revision'):
            self.prepare()
