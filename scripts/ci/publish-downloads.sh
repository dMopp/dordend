#!/usr/bin/env bash
# Only call after all checks and asset preparation succeed on trusted main.
set -euo pipefail
assets=${1:?usage: publish-downloads.sh ASSETS_DIRECTORY COMMIT}
commit=${2:?usage: publish-downloads.sh ASSETS_DIRECTORY COMMIT}
: "${GH_REPO:?Set GH_REPO to owner/repository}"
[[ "$commit" =~ ^[0-9a-f]{40}$ ]]
assets=$(realpath "$assets")
(cd "$assets" && sha256sum --check SHA256SUMS)
test -s "$assets/release-notes.md"
# Fail before any mutation if the prepared identity differs from the tag target.
python3 - "$assets/source.json" "$commit" <<'PY'
import json, sys
assert json.load(open(sys.argv[1]))['commit'] == sys.argv[2]
PY
files=(nordend-debian13-amd64.deb nordend-fedora44-x86_64.rpm
       nordend-arch-x86_64.pkg.tar.zst nordend-tumbleweed-x86_64.rpm
       nordend-void-glibc-x86_64.tar.gz nordend-source.tar.gz source.json SHA256SUMS)
for file in "${files[@]}"; do test -s "$assets/$file"; done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if gh api "repos/$GH_REPO/releases/tags/development" > "$work/release.json" 2> "$work/error"; then
    python3 - "$work/release.json" <<'PY'
import json, sys
assert not json.load(open(sys.argv[1])).get('immutable', False), 'Rolling downloads need a mutable prerelease'
PY
    # Rerunning an older successful workflow must not roll back a newer download.
    relationship=$(gh api "repos/$GH_REPO/compare/development...$commit" --jq '.status')
    case "$relationship" in
        ahead|identical) ;;
        behind) echo 'A newer development build is already published; keeping it.'; exit 0 ;;
        *) echo "Development history diverged; refusing to replace it: $relationship" >&2; exit 1 ;;
    esac
    # Upload the checksum manifest last. Readers can detect a partial update and retry.
    for file in "${files[@]}"; do
        gh release upload development "$assets/$file" --clobber
    done
    gh api --method PATCH "repos/$GH_REPO/git/refs/tags/development" \
        -f sha="$commit" -F force=true > /dev/null
    gh release edit development --title 'Latest development build' --prerelease --latest=false \
        --notes-file "$assets/release-notes.md" --draft=false
elif grep -Fq '(HTTP 404)' "$work/error"; then
    paths=()
    for file in "${files[@]}"; do paths+=("$assets/$file"); done
    gh release create development "${paths[@]}" --target "$commit" \
        --title 'Latest development build' --prerelease --latest=false \
        --notes-file "$assets/release-notes.md"
    # Recover a tag left behind by an interrupted first publication, too.
    gh api --method PATCH "repos/$GH_REPO/git/refs/tags/development" \
        -f sha="$commit" -F force=true > /dev/null
else
    cat "$work/error" >&2
    exit 1
fi
