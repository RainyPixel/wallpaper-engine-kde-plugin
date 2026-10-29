#!/usr/bin/env bash
# End-to-end test for `sceneviewer --screenshot`.
#
#   test_screenshot.sh <sceneviewer>
#
# Needs a display (Wayland or X11), a Vulkan device, Wallpaper Engine assets and
# one scene, so it skips (exit 77) unless both of these are set:
#   WEKDE_E2E_ASSETS  path to Wallpaper Engine's "assets" directory
#   WEKDE_E2E_SCENE   path to a scene.pkg (or an unpacked scene.json)
#
# Every viewer run is bounded by `timeout`, so a hang fails the test instead of
# blocking it.
set -u

viewer=${1:?usage: test_screenshot.sh <sceneviewer>}
skip() { echo "SKIP: $*"; exit 77; }
[ -n "${WEKDE_E2E_ASSETS:-}" ] && [ -n "${WEKDE_E2E_SCENE:-}" ] ||
    skip "set WEKDE_E2E_ASSETS and WEKDE_E2E_SCENE to run"
[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ] || skip "no display"
[ -x "$viewer" ] || { echo "FAIL: $viewer is not executable"; exit 1; }

W=640 H=360
run_limit=${WEKDE_E2E_RUN_LIMIT:-60}
tmp=$(mktemp -d) || exit 1
cleanup() { chmod -R u+rwx "$tmp" 2>/dev/null; rm -rf "$tmp"; }
trap cleanup EXIT

failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
check_validation() {
    if grep -Eq 'Validation Error|SYNC-HAZARD|VUID-' "$tmp/log"; then
        fail "Vulkan validation reported an error"
        cat "$tmp/log"
    fi
}

# Runs the viewer once; prints its exit status. Output goes to $tmp/log.
shoot() {
    timeout -k 5 "$run_limit" "$viewer" -R "${W}x${H}" -f 30 \
        --shot-delay 1 --shot-timeout 20 -S "$1" \
        "$WEKDE_E2E_ASSETS" "$WEKDE_E2E_SCENE" >"$tmp/log" 2>&1
    echo $?
}

# A valid dump is exactly "P6\n<W> <H>\n255\n" followed by W*H RGB triples.
valid_ppm() {
    local header="P6
$W $H
255"
    [ "$(head -c ${#header} "$1")" = "$header" ] &&
        [ "$(stat -c %s "$1")" -eq $(( ${#header} + 1 + W * H * 3 )) ]
}

# 1. Fresh output: written, complete, exit 0.
out=$tmp/fresh.ppm
rc=$(shoot "$out")
check_validation
if [ "$rc" -ne 0 ]; then
    fail "fresh output: exit $rc"; cat "$tmp/log"
elif ! valid_ppm "$out"; then
    fail "fresh output: not a complete ${W}x${H} PPM"
else
    echo "ok: fresh output"
fi

# 2. Existing output: replaced by a new frame. The stale file must not count as
#    success on its own, and none of it may survive.
out=$tmp/existing.ppm
head -c 100000 /dev/zero | tr '\0' 'x' >"$out"
rc=$(shoot "$out")
check_validation
if [ "$rc" -ne 0 ]; then
    fail "existing output: exit $rc"; cat "$tmp/log"
elif ! valid_ppm "$out"; then
    fail "existing output: stale content was not replaced by a complete PPM"
else
    echo "ok: existing output"
fi

# 3. Write failure, missing directory: non-zero exit, nothing created.
out=$tmp/missing/dir/frame.ppm
rc=$(shoot "$out")
check_validation
if [ "$rc" -ne 1 ]; then
    fail "missing directory: expected exit 1, got $rc"; cat "$tmp/log"
elif [ -e "$out" ] || [ -e "$tmp/missing" ]; then
    fail "missing directory: something was created"
else
    echo "ok: write failure (missing directory)"
fi

# 4. Write failure, read-only directory: non-zero exit, old file untouched.
if [ "$(id -u)" -ne 0 ]; then
    mkdir "$tmp/ro" && echo old >"$tmp/ro/frame.ppm" && chmod 555 "$tmp/ro"
    rc=$(shoot "$tmp/ro/frame.ppm")
    check_validation
    chmod 755 "$tmp/ro"
    if [ "$rc" -ne 1 ]; then
        fail "read-only directory: expected exit 1, got $rc"; cat "$tmp/log"
    elif [ "$(cat "$tmp/ro/frame.ppm")" != old ] || [ "$(ls -A "$tmp/ro" | wc -l)" -ne 1 ]; then
        fail "read-only directory: existing file changed or a temporary was left"
    else
        echo "ok: write failure (read-only directory)"
    fi
fi

# 5. A pending request without a scene must time out, including during teardown.
out=$tmp/no-frame.ppm
timeout -k 5 "$run_limit" "$viewer" -R "${W}x${H}" -f 30 \
    --shot-delay 0.5 --shot-timeout 2 -S "$out" \
    "$WEKDE_E2E_ASSETS" "$tmp/absent-scene.json" >"$tmp/log" 2>&1
rc=$?
check_validation
if [ "$rc" -ne 2 ]; then
    fail "no frame: expected exit 2, got $rc"; cat "$tmp/log"
elif [ -e "$out" ]; then
    fail "no frame: an output file was created"
else
    echo "ok: no frame times out"
fi

[ "$failures" -eq 0 ] && echo PASS || echo "$failures check(s) failed"
[ "$failures" -eq 0 ]
