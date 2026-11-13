#!/usr/bin/env bash
# Export only committed source and its identity for container package builds.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
output=${1:?usage: scripts/ci/export-source.sh OUTPUT_DIRECTORY}
mkdir -p "$output"
output=$(realpath "$output")
[[ "$output" != "$root" && "$output" != "$root/"* ]]
if [[ -n $(ls -A "$output") ]]; then
    echo 'Use an empty output directory so old packages cannot be installed by mistake.' >&2
    exit 1
fi
if [[ -n $(git status --porcelain --untracked-files=normal) ]]; then
    echo 'Commit the checkout first: the package must match the reported Git revision.' >&2
    exit 1
fi
mkdir -p "$output/input"
git archive --format=tar --prefix=rummage-checkout/ HEAD | gzip -n > "$output/input/source.tar.gz"
python3 - "$output/input" <<'PY'
import hashlib, json, os, re, subprocess, sys
from pathlib import Path
out = Path(sys.argv[1])
version = re.search(r'project\(rummage VERSION ([0-9.]+)', Path('CMakeLists.txt').read_text())[1]
metadata = {
    'commit': subprocess.check_output(['git','rev-parse','HEAD'], text=True).strip(),
    'timestamp': subprocess.check_output(['git','show','-s','--format=%cd','--date=format-local:%Y%m%d%H%M%S','HEAD'], text=True, env={**os.environ, 'TZ':'UTC'}).strip(),
    'version': version,
    'sha256': hashlib.sha256((out/'source.tar.gz').read_bytes()).hexdigest(),
}
(out/'source.json').write_text(json.dumps(metadata, indent=2)+'\n')
PY
