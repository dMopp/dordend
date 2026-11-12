# CI and package validation

The fork's [workflow](../.github/workflows/ci.yml) runs on pull requests, pushes to `main`,
and manual dispatch. It currently validates **x86_64 Linux with glibc**. The flake still
exposes aarch64 packages, but this workflow does not claim to build or run them. Debian 13,
Fedora 44, Arch Linux and openSUSE Tumbleweed are also built as native packages. Void musl, older Debian/Ubuntu
releases, other RPM distributions and signed/stable releases are outside the validated scope.

## Nix

The `Nix build, tests and installed package` job runs:

```sh
nix flake check --print-build-logs
nix build --print-build-logs
```

Checks include:

- `tests`: build the complete unit suite and the private D-Bus file-picker and notification
  integration tests, with deterministic fonts, a MIME database, software rendering and
  archive tools. Allocator-sensitive tests run in a separate process, not after the rest
  of the suite has changed glibc's allocation thresholds.
- `smoke`: inspect the actual installed package, validate the installed example config and
  feature syntax, check desktop/portal files and their paths, then render a window under
  headless Weston. Merely surviving startup is insufficient: the Wayland log must show
  the application attaching a buffer.
- `nixos-module`: evaluate module enablement, portal selection, coexistence with other
  portal backends, and invalid configurations.

The ordinary package remains a release build without test executables. The separate
`checks.<system>.tests` derivation enables tests; it does not skip them in exchange for a
successful package build. Tests that probe optional kernel capabilities retain their
existing capability checks.

To run just one check locally:

```sh
nix build .#checks.x86_64-linux.tests --no-link --print-build-logs
nix build .#checks.x86_64-linux.smoke --no-link --print-build-logs
nix build .#checks.x86_64-linux.nixos-module --no-link --print-build-logs
```

To share the Nix version, send the fork reference. With flakes enabled, the recipient can
run it directly or install it into their profile:

```sh
nix run github:barrulus/bordend-fm
nix profile install github:barrulus/bordend-fm
```

Append `/COMMIT_SHA` to that reference to select an exact revision. This fetches the source
and builds Nordend if a matching binary is not cached. The Nix job does not currently upload
a portable binary closure; copying only the executable omits its `/nix/store` dependencies.

## Void Linux

The `Void glibc package and clean install` job uses Docker and Void's `xbps-src`. It packages
**the exact committed checkout**, including the PR merge commit when GitHub checks one out.
It does not fetch upstream's v0.5.0 and mistake that build for a test of this fork.

From a clean, committed checkout, with Docker, Git, Bash, Python 3, tar/gzip tools and
coreutils available:

```sh
bash scripts/ci/void.sh /tmp/nordend-void-package
```

Choose a new or empty output directory outside the checkout. Uncommitted changes are rejected because
`git archive HEAD` must match the revision recorded with the artifact. `CI_JOBS` sets build
parallelism (default: 2).

The script:

1. Archives the checkout and records its commit, commit timestamp, application version,
   and SHA-256 in `input/source.json`.
2. Starts a disposable Void container. Inside it, `xbps-src` uses Void's container-only
   `ethereal` build mode; no privileged Docker container or host chroot is required.
3. Copies the checked-in Void recipe, changing only source/version metadata to use the
   verified checkout archive. The snapshot version is the application version followed by
   the UTC commit timestamp. The source is preseeded in xbps-src's checksum-verified cache;
   a cache miss fails rather than falling back to an unrelated release.
4. Builds the `.xbps`, then starts a **fresh container** and installs that package through
   XBPS. The build container's compiler and development dependencies cannot hide missing
   package dependencies. The Void recipe's unit tests remain disabled; the complete suite
   is exercised by the Nix job.
5. Checks package integrity and linked libraries, validates installed resources and config,
   then installs the compositor/test fixtures and performs the headless Wayland launch.

To share a Void build, download its artifact ZIP from a successful GitHub Actions run and
send the ZIP. The recipient should extract it, enter `void-package/`, and keep the entire `packages/` directory
together, including the repository index.

The artifact contains the `.xbps` and repository index in `packages/`, the effective
template, `source.json`, and the workflow build log. On a Void machine, install a downloaded
snapshot with:

```sh
sudo xbps-install -R /path/to/artifact/packages nordend
```

These are unsigned development snapshots, not an official Void repository or published fork
release. Their package version distinguishes them from upstream v0.5.0; the application
still reports its CMake version. `source.json` identifies the exact source commit.

The checked-in [release template](../packaging/void/nordend/template) still describes the
upstream v0.5.0 release, with its original checksum. The checkout builder deliberately
reuses its dependency and install rules. A future fork release needs an explicit fork tag,
version, archive URL and checksum before changing that release recipe or publishing packages.

The container digest and void-packages commit are pinned in
[`scripts/ci/void-env`](../scripts/ci/void-env). Void's binary repositories are rolling, so
those dependencies can still change; this is repeatable build tooling, not a bit-for-bit
reproducibility claim. Update the pins deliberately and rerun both build and clean-install
checks. Never run `void-build.sh` or `void-install.sh` directly on a host; `void.sh` supplies
the disposable containers.

## Debian and Fedora

The `Debian 13 package and clean install` and `Fedora 44 package and clean install` jobs
build native **amd64 DEB** and **x86_64 RPM** snapshots. These target Debian 13 (trixie) and
Fedora 44 specifically; the file extension alone does not imply compatibility with Ubuntu,
older Debian, RHEL or other RPM systems. Nordend requires sdbus-c++ 2 or newer.

From a clean committed checkout, with the same host tools as the Void builder:

```sh
bash scripts/ci/native.sh deb /tmp/nordend-deb-package
bash scripts/ci/native.sh rpm /tmp/nordend-rpm-package
```

`CI_JOBS` controls parallelism (default 2). Each output directory must be empty and outside
the checkout. The shared source exporter records the exact commit and archive SHA-256.
Debian uses [`packaging/debian`](../packaging/debian) with `dpkg-buildpackage`; Fedora uses
[`packaging/rpm/nordend.spec`](../packaging/rpm/nordend.spec) with `rpmbuild`. Runtime library
dependencies are generated by the native packaging tools. MIME data and the icon theme are
explicit dependencies. Both packages include bundled third-party license notices.

The build runs in a disposable container, followed by installation through apt or dnf in a
fresh container. Package integrity and shared libraries are checked before installing any
test fixtures. The common installed-resource checks and headless Weston render check then
run against `/usr/bin/nordend`. Unit tests run in the separate Nix job; these two jobs test
the native build and installed package. No privileged container is needed.

The artifacts contain binary packages (plus Debian build metadata or Fedora source/debug
RPMs), the effective recipe, the exact source archive and `source.json`, and the build log.
Download and unpack the matching artifact from the workflow run, then enter its
`native-package/` directory. On the matching distro:

```sh
# Debian 13: choose the main package, not the optional debug-symbol package.
sudo apt install ./packages/nordend_*.deb
# Fedora 44: choose the main binary RPM, not the source/debug RPMs.
sudo dnf install ./packages/nordend-[0-9]*.x86_64.rpm
```

Snapshots are unsigned development packages, not an apt/dnf repository or official release.
Their versions include the UTC source commit timestamp and abbreviated hash; the application
continues to report its CMake version. A later commit produces a newer snapshot when the
commit timestamp increases. Container images are pinned in
[`scripts/ci/native-env`](../scripts/ci/native-env), but apt/dnf repositories still receive
updates, so builds are not claimed to be bit-for-bit reproducible. Run `native.sh` rather
than invoking `native-build.sh` or `native-install.sh` on the host.

## Arch Linux and openSUSE Tumbleweed

These are separate **x86_64 rolling-distribution** targets. Arch uses the
[`PKGBUILD`](../packaging/arch/PKGBUILD) with `makepkg`; Tumbleweed builds the shared
[RPM recipe](../packaging/rpm/nordend.spec) using openSUSE dependencies and RPM macros.
Neither the Fedora RPM nor the Tumbleweed RPM is claimed to support Leap, RHEL or other
RPM distributions. Ubuntu is not a CI target.

```sh
bash scripts/ci/native.sh arch /tmp/nordend-arch-package
bash scripts/ci/native.sh tumbleweed /tmp/nordend-tumbleweed-package
```

Both builders use pinned base images and fully update the disposable container first to
avoid partial upgrades against rolling repositories. Package repositories still change over
time. Arch runs `makepkg` as an ordinary build user; root is used only for installing build
dependencies. Its recipe gets the archive's verified SHA-256 and a source-derived snapshot
version. The checked-in PKGBUILD is a checkout-build template, not an AUR submission.

A second, fresh container installs the resulting package with pacman or zypper. CI checks
package integrity, shared-library dependencies, installed assets and a rendered window under
headless Weston. Test fixtures are installed after the runtime dependency checks. The complete
unit suite continues to run in Nix. Both jobs must pass before any development downloads are
published, and the release preparer requires all five native package identities to match.

The README provides stable links and installation commands. Arch and Tumbleweed users should
fully update their systems before installing these snapshots. These are unsigned local
packages, not configured pacman/zypper repositories; Tumbleweed's install command explicitly
allows the unsigned local RPM without changing global signature settings.

## Automatic development downloads

The README has stable links to the `development` prerelease and fixed asset names. Once all
checks succeed, `prepare-downloads.py` checks that all five package artifacts identify the
same source commit and checksum. It selects only the main DEB/RPM, bundles the Void package
with its repository index, and adds the source archive, `source.json` and `SHA256SUMS`.
Release install instructions are copied from the marked README section, so they stay in sync.
Preparation runs on PRs too; the prepared bundle is available as a workflow artifact.

On trusted `main` runs, the publication job creates or updates **Latest development build**.
It replaces the assets, moves the `development` tag to the tested commit, and updates the
release notes with a link to the CI run. It is explicitly a prerelease, not the repository's
latest stable release. Nix uses the same moving tag as its source reference; this does not
publish a Nix binary closure. No README commits are needed for subsequent builds.

Main runs are serialized without automatic cancellation, preventing overlapping publication.
PR runs still cancel superseded checks. Rerunning an older successful main workflow cannot
roll downloads back to an ancestor of the already-published commit; divergent history fails
for maintainer review. Failed build/check/preparation jobs leave the existing
downloads untouched. Asset replacement uses separate GitHub API calls, not an atomic swap:
if publication is interrupted, rerun the job to finish it. Checksums are uploaded last;
retry a download if verification fails during an update. The rolling prerelease must remain
mutable; repositories enforcing immutable releases need a different publishing strategy.

Release assets stay available until replaced or removed, independently of the 14-day CI
artifact retention. They are unsigned snapshots and must not be presented as stable releases,
an official distribution repository, or packages for untested distributions such as Leap or Ubuntu.

## GitHub setup and scope

GitHub Actions must be enabled on the fork. Build and preparation jobs use read-only repository
permissions, pinned action commits, and `pull_request`, not `pull_request_target`. Only the
publication job has `contents: write`, using the built-in GitHub token; no extra secret is
needed. It runs exclusively on pushes to `main` or manual runs targeting `main`, after all
six checks and download preparation pass. It never commits to a branch.

After a successful run, configure a branch ruleset for `main` requiring these checks:

- `Nix build, tests and installed package`
- `Void glibc package and clean install`
- `Debian 13 package and clean install`
- `Fedora 44 package and clean install`
- `Arch Linux package and clean install`
- `openSUSE Tumbleweed package and clean install`

Package artifacts are retained for 14 days. Neither a passing container check nor a headless
window proves integration with every compositor, GPU, removable drive, or desktop portal
configuration. Those remain real-desktop checks before release. Native ARM runners, Void
musl compatibility, additional distribution recipes, caching, and signed release artifacts
can be added separately without weakening these required checks.
