"""Compare local icon references, with no resvg dependency or recapture path."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--renderer', default='build/rummage-svg-render')
    parser.add_argument('--size', type=int, choices=(16, 48, 128))
    parser.add_argument('--corpus', type=Path, default=Path('tests/data/svg-corpus'))
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    manifest = args.corpus / 'manifest.tsv'
    if not manifest.is_file():
        print('Local SVG reference capture is missing; skipping corpus comparison.')
        return
    rows = list(csv.DictReader(manifest.open(), delimiter='\t'))
    results = []
    hashes = {}
    with tempfile.TemporaryDirectory(prefix='rummage-svg-corpus-') as temp:
        output = Path(temp) / 'actual.png'
        for size in (16, 48, 128):
            if args.size and args.size != size:
                continue
            checked = []
            stale = []
            for row in rows:
                if int(row['size']) != size:
                    continue
                source = row['source']
                if source not in hashes:
                    try:
                        hashes[source] = hashlib.sha256(Path(source).read_bytes()).hexdigest()
                    except OSError:
                        hashes[source] = None
                if hashes[source] != row['sha256']:
                    stale.append(source)
                    continue
                start = time.perf_counter()
                try:
                    run = subprocess.run([args.renderer, source, str(size), str(output), str(args.corpus / row['reference'])],
                                         capture_output=True, text=True, timeout=30)
                    seconds, maximum, fraction, passed = run.stdout.strip().split('\t')
                    item = dict(source=source, seconds=float(seconds), maximum=int(maximum),
                                fraction=float(fraction), passed=passed == '1', error=run.stderr.strip())
                except (subprocess.TimeoutExpired, ValueError) as error:
                    detail = str(error)
                    if isinstance(error, ValueError):
                        detail = f'exit {run.returncode}: {run.stderr.strip() or error}'
                    item = dict(source=source, seconds=None, maximum=255, fraction=1.0, passed=False, error=detail)
                item.update(wall_seconds=time.perf_counter() - start,
                            reference_seconds=float(row.get('render_seconds', row['seconds'])),
                            reference_wall_seconds=float(row['seconds']))
                checked.append(item)
            total = len(checked)
            passed = sum(item['passed'] for item in checked)
            seconds = sum(item['seconds'] for item in checked if item['seconds'] is not None)
            timing_failures = sum(item['seconds'] is None for item in checked)
            baseline = sum(item['reference_seconds'] for item in checked)
            summary = dict(size=size, checked=total, passed=passed, percent=100 * passed / total if total else 0,
                           seconds=seconds, reference_seconds=baseline,
                           ratio=seconds / baseline if baseline and not timing_failures else None,
                           timing_failures=timing_failures, stale=stale, icons=checked)
            results.append(summary)
            print(f"{size}px: {passed}/{total} ({summary['percent']:.2f}%), {len(stale)} stale; "
                  f"parse+render {seconds:.6f}s / resvg {baseline:.6f}s; "
                  f"ratio={summary['ratio']}, {timing_failures} missing timings")
            for source in stale:
                print(f'  STALE {source}')
            for item in sorted(checked, key=lambda x: (x['fraction'], x['maximum']), reverse=True)[:20]:
                print(f"  max={item['maximum']} bad={item['fraction']:.3%} {item['source']} {item['error']}")
    if args.report:
        args.report.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
