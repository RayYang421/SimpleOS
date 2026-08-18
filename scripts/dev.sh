#!/bin/sh
# Runs a command in the build container, with the repository bind-mounted at
# /work.
#
#   scripts/dev.sh make EXTRA_CFLAGS=-Werror all bootloader
#   scripts/dev.sh make test
#   TEST_WAIT_SCALE=3 scripts/dev.sh make test
#   scripts/dev.sh
#   scripts/dev.sh --rebuild
#

set -eu

IMAGE=${DEV_IMAGE:-osc-build}
DOCKER=${DOCKER:-docker}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)

        repo=$(cd -- "$script_dir/.." && pwd -W)
        MSYS_NO_PATHCONV=1
        export MSYS_NO_PATHCONV
        user_args=""
        ;;
    *)
        repo=$(cd -- "$script_dir/.." && pwd)
        # Build products land in a bind mount, so they should belong to whoever
        # ran this rather than to root.
        user_args="--user $(id -u):$(id -g)"
        ;;
esac

rebuild=0
if [ "${1:-}" = "--rebuild" ]; then
    rebuild=1
    shift
fi

if [ "$rebuild" -eq 1 ] || ! "$DOCKER" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Building $IMAGE..." >&2
    "$DOCKER" build -t "$IMAGE" "$repo"
fi

# A terminal only when there is one to attach, so this stays usable from a
# script or a CI job.
tty_args=""
if [ -t 0 ] && [ -t 1 ]; then
    tty_args="-it"
fi

# TEST_WAIT_SCALE is passed through: every wait in the end-to-end test is
# wall-clock, but what it waits for is the guest making progress, and a
# container on a laptop is slower than the machine the delays were chosen on.
# Raise it if the test fails on timing rather than on a check.
env_args=""
if [ -n "${TEST_WAIT_SCALE:-}" ]; then
    env_args="-e TEST_WAIT_SCALE=$TEST_WAIT_SCALE"
fi

if [ "$#" -eq 0 ]; then
    set -- /bin/bash
fi

# shellcheck disable=SC2086  # the *_args are deliberately word-split
exec "$DOCKER" run --rm $tty_args $user_args $env_args \
     -v "$repo:/work" -w /work "$IMAGE" "$@"
