#!/usr/bin/env bash
set -euo pipefail
prefix=${1:?usage: check-wayland.sh PREFIX}
work=$(mktemp -d)
compositor=
cleanup() {
    if [[ -n "$compositor" ]]; then
        kill "$compositor" 2>/dev/null || true
        wait "$compositor" 2>/dev/null || true
    fi
    if [[ ${status:-1} != 0 ]]; then
        cat "$work/weston.log" "$work/rummage.log" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT
export XDG_RUNTIME_DIR="$work/runtime"
export XDG_CONFIG_HOME="$work/config" XDG_CACHE_HOME="$work/cache"
export XDG_DATA_HOME="$work/data" XDG_STATE_HOME="$work/state"
mkdir -m 700 "$XDG_RUNTIME_DIR"
export WAYLAND_DISPLAY=rummage-ci
export LIBGL_ALWAYS_SOFTWARE=1
weston --backend=headless --renderer=pixman --socket="$WAYLAND_DISPLAY" \
    --idle-time=0 --no-config --log="$work/weston.log" &
compositor=$!
for ((attempt=0; attempt<100; ++attempt)); do
    [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break
    kill -0 "$compositor"
    sleep 0.1
done
test -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
# A client that crashes or cannot connect exits before timeout. Require a real buffer attach,
# too: surviving five seconds without ever drawing a window is not a successful launch.
exit_code=0
dbus-run-session -- timeout 5s env WAYLAND_DEBUG=1 \
    "$prefix/bin/rummage" --test-scene > "$work/rummage.log" 2>&1 || exit_code=$?
[[ $exit_code == 124 ]]
grep -E 'wl_surface[^ ]*\.attach\(' "$work/rummage.log"
status=0
printf 'Installed rummage rendered a window under headless Weston.\n'
