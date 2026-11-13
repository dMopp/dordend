#!/usr/bin/env bash
# Rebuilds rummage's icon fonts, third_party/tabler-icons/tabler-icons{,-filled}.ttf: the
# official Tabler Icons webfont (npm package @tabler/icons-webfont), cut down to the glyphs
# listed in icons.txt and icons-filled.txt, glyph names kept. Run it after changing a list or
# the version below, then update the sha256 in THIRD_PARTY.md.
#
# Needs curl, tar and fonttools (pyftsubset).
set -euo pipefail
cd "$(dirname "$0")/.."

version=3.50.0
# sha256 of icons-webfont-$version.tgz (npm's shasum: 5580c8af3cfde6ed42b03bd6e33b9e73397ff79d)
sha256=1afa9b3764607de419b4eeff296f2328361a5ba605ebd35812832f61ddbf070e
out=third_party/tabler-icons

command -v pyftsubset >/dev/null 2>&1 || { echo "error: pyftsubset not found (fonttools)" >&2; exit 127; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tarball="$work/icons-webfont-$version.tgz"
curl -sSfL -o "$tarball" "https://registry.npmjs.org/@tabler/icons-webfont/-/icons-webfont-$version.tgz"
echo "$sha256  $tarball" | sha256sum --check --quiet
tar xzf "$tarball" -C "$work" package/LICENSE package/dist/fonts/tabler-icons.ttf \
    package/dist/fonts/tabler-icons-filled.ttf

# One comma-separated list of the names in a file, comments and blank lines left out.
names() { grep -v -e '^#' -e '^[[:space:]]*$' "$1" | paste -sd, -; }

subset() {
    pyftsubset "$work/package/dist/fonts/$1" --output-file="$out/$1" \
        --glyphs="$(names "$out/$2")" --glyph-names --notdef-outline --layout-features= \
        --name-IDs='*' --name-languages='*'
}
subset tabler-icons.ttf icons.txt
subset tabler-icons-filled.ttf icons-filled.txt
cp "$work/package/LICENSE" "$out/LICENSE"

sha256sum "$out"/*.ttf
