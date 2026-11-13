#!/usr/bin/env python3
"""Stage the exact checkout archive in xbps-src's verified source cache."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

source, packages = map(Path, sys.argv[1:])
metadata = json.loads((source / "source.json").read_text())
assert re.fullmatch(r"[0-9a-f]{40}", metadata["commit"])
assert re.fullmatch(r"[0-9.]+", metadata["version"])
assert re.fullmatch(r"[0-9]{14}", metadata["timestamp"])
version = metadata["version"] + "." + metadata["timestamp"]
archive = source / "source.tar.gz"
checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
assert checksum == metadata["sha256"], "Source archive differs from recorded checkout"
name = f"rummage-{version}.tar.gz"
cache = packages / "hostdir" / "sources" / f"rummage-{version}"
cache.mkdir(parents=True)
shutil.copyfile(archive, cache / name)
path = packages / "srcpkgs" / "rummage" / "template"
text = path.read_text()
replacements = {
    "version": version,
    "revision": "1",
    # Cache-only source: a missing archive must fail, not fetch a different source tree.
    "distfiles": f'"https://invalid.invalid/local-checkout/{name}"',
    "checksum": checksum,
}
for key, value in replacements.items():
    text, count = re.subn(rf"^{key}=.*$", f"{key}={value}", text, flags=re.MULTILINE)
    assert count == 1, f"Missing or duplicate {key} in template"
text += '\nwrksrc="rummage-checkout"\n'
path.write_text(text)
print(f"Packaging checkout {metadata['commit']} as rummage-{version}_1", flush=True)
