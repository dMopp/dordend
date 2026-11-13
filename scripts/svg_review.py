"""Build a fixed native-size SVG review from the preserved local captures."""
import argparse
import base64
import csv
import hashlib
import html
import json
from pathlib import Path
import struct
import subprocess


def thumbnail(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'Not a PNG: {path}')
    width, height = struct.unpack('>II', data[16:24])
    uri = 'data:image/png;base64,' + base64.b64encode(data).decode()
    return f'<img width="{width}" height="{height}" src="{uri}" alt="icon">'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', default='build/rummage-svg-render')
    parser.add_argument('--corpus', type=Path, default=Path('tests/data/svg-corpus'))
    parser.add_argument('--selection', type=Path, default=Path('tests/data/svg-review.tsv'))
    parser.add_argument('--output', type=Path, default=Path('build/svg-review'))
    args = parser.parse_args()
    with (args.corpus / 'manifest.tsv').open() as source:
        manifest = {r['reference']: r for r in csv.DictReader(source, delimiter='\t')}
    with args.selection.open() as source:
        selection = list(csv.DictReader(source, delimiter='\t'))
    jobs = []
    for entry in selection:
        for size in (16, 48, 128):
            key = f"{entry['theme']}/{size}/{entry['path']}"
            row = manifest[key]
            actual_hash = hashlib.sha256(Path(row['source']).read_bytes()).hexdigest()
            if actual_hash != row['sha256'] or not (args.corpus / key).is_file():
                raise ValueError(f'Missing capture or stale source: {key}')
            jobs.append((entry, size, row))
    args.output.mkdir(parents=True, exist_ok=True)
    parts = ['''<!doctype html><meta charset="utf-8"><title>SVG native-size review</title>
<style>body{font:15px system-ui;margin:24px;color:#202530;background:#f4f5f7}
table{border-collapse:collapse}th,td{border:1px solid #b8bec6;padding:12px;vertical-align:top}
td:first-child{max-width:290px;overflow-wrap:anywhere}.pair{display:flex;gap:12px;align-items:start}
.surface{display:flex;align-items:center;justify-content:center;padding:12px;min-width:24px;
min-height:24px;background:#fff}.dark .surface{background:#222}.checker .surface{
background:repeating-conic-gradient(#eee 0% 25%,#bbb 0% 50%) 0/12px 12px}
.label{font-size:12px;margin:6px 0}.zoom img{width:256px;height:auto;image-rendering:pixelated}
button{padding:8px;margin:0 8px 16px 0}details{margin-top:10px;font-size:12px}</style>
<h1>SVG native-size review</h1><p>Each pair: preserved reference, then current renderer.
Images use their native pixel dimensions at 100% browser zoom. Pixel metrics remain diagnostic;
judge identity, silhouette, content and contrast using the documented acceptance policy.</p>
<button onclick="document.body.className=''">Light</button>
<button onclick="document.body.className='dark'">Dark</button>
<button onclick="document.body.className='checker'">Checker</button>
<table><tr><th>Fixed selection</th><th>16 px</th><th>48 px</th><th>128 px</th></tr>''']
    inventory = []
    for index, (entry, size, row) in enumerate(jobs):
        if size == 16:
            parts.append('<tr><td><b>' + html.escape(entry['theme'] + '/' + entry['path']) +
                         '</b><p>' + html.escape(entry['purpose']) + '</p></td>')
        output = args.output / f'icon-{index // 3}-{size}.png'
        reference = args.corpus / row['reference']
        run = subprocess.run([args.renderer, row['source'], str(size), str(output), str(reference)],
                             capture_output=True, text=True, check=True, timeout=30)
        seconds, maximum, fraction, passed = run.stdout.strip().split('\t')
        inventory.append(dict(theme=entry['theme'], path=entry['path'], size=size,
                              source=row['source'], source_sha256=row['sha256'],
                              reference=row['reference'], output=output.name,
                              seconds=float(seconds), maximum=int(maximum), fraction=float(fraction),
                              pixel_comparison_passed=passed == '1', warning=run.stderr.strip()))
        images = [thumbnail(reference), thumbnail(output)]
        parts.append('<td><div class="pair">' + ''.join(
            f'<div><div class="label">{label}</div><div class="surface">{im}</div></div>'
            for label, im in zip(('Reference', 'Current'), images)) + '</div>')
        parts.append(f'<details><summary>Pixel difference: max {maximum}, '
                     f'{100 * float(fraction):.3f}% over 24</summary><div class="pair zoom">' +
                     ''.join(f'<div class="surface">{im}</div>' for im in images) + '</div></details></td>')
        if size == 128:
            parts.append('</tr>')
    parts.append('</table>')
    (args.output / 'review.html').write_text(''.join(parts))
    record = dict(renderer_sha256=hashlib.sha256(Path(args.renderer).read_bytes()).hexdigest(),
                  selection_sha256=hashlib.sha256(args.selection.read_bytes()).hexdigest(),
                  visual_review='Pending inspection; rendering success is not visual acceptance.',
                  icons=inventory)
    (args.output / 'inventory.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'{len(selection)} icons, {len(inventory)} renders: {args.output / "review.html"}')


if __name__ == '__main__':
    main()
