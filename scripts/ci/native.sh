#!/usr/bin/env bash
# Build and test a native package from the exact committed checkout.
set -euo pipefail
format=${1:?usage: native.sh deb|rpm|arch|tumbleweed OUTPUT_DIRECTORY}
output=${2:?usage: native.sh deb|rpm|arch|tumbleweed OUTPUT_DIRECTORY}
root=$(git rev-parse --show-toplevel)
cd "$root"
# shellcheck source=scripts/ci/native-env
source scripts/ci/native-env
case "$format" in
    deb) image=$DEBIAN_IMAGE ;;
    rpm) image=$FEDORA_IMAGE ;;
    arch) image=$ARCH_IMAGE ;;
    tumbleweed) image=$TUMBLEWEED_IMAGE ;;
    *) echo 'Expected deb, rpm, arch or tumbleweed' >&2; exit 1 ;;
esac
bash scripts/ci/export-source.sh "$output"
output=$(realpath "$output")
version=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$output/input/source.json")
docker run --rm --platform linux/amd64 \
    -v "$root:/source:ro" -v "$output/input:/input:ro" -v "$output:/output" \
    -e CI_JOBS="${CI_JOBS:-2}" "$image" bash /source/scripts/ci/native-build.sh "$format"
# No build dependencies survive into this installation test.
docker run --rm --platform linux/amd64 \
    -v "$root:/source:ro" -v "$output/packages:/packages:ro" \
    "$image" bash /source/scripts/ci/native-install.sh "$format" "$version"
