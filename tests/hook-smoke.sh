#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
prefix=$(mktemp -d "${TMPDIR:-/tmp}/uur-wine-test.XXXXXX")
recovery_pid=""
trap '[[ -z "$recovery_pid" ]] || kill "$recovery_pid" >/dev/null 2>&1 || true; WINEPREFIX="$prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf -- "$prefix"' EXIT

# Debian-based Wine packages keep wineserver outside the default PATH even
# though /usr/bin/wine invokes it normally.
if ! command -v wineserver >/dev/null 2>&1; then
    for wine_dir in /usr/lib/x86_64-linux-gnu/wine /usr/lib64/wine; do
        if [[ -x "$wine_dir/wineserver" ]]; then
            PATH="$wine_dir:$PATH"
            export PATH
            break
        fi
    done
fi

for tool in wine wineboot wineserver xvfb-run timeout python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'missing hook smoke-test dependency: %s\n' "$tool" >&2
        exit 2
    }
done

WINEPREFIX="$prefix" WINEDEBUG=-all xvfb-run -a wineboot --init
python3 "$project_root/tests/capture-recovery.py" "$prefix/drive_c" &
recovery_pid=$!
set +e
output=$(
    cd "$project_root/build/hook" &&
        WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES=wevtapi=n \
            timeout 30s xvfb-run -a wine ./selftest.exe --native-removal 2>&1
)
status=$?
set -e

printf '%s\n' "$output"

# Some xvfb-run versions return 1 when their Xvfb child has already exited during
# cleanup, even though the wrapped process completed successfully. Keep checking
# the self-test's terminal success marker so genuine Wine or hook failures remain
# fatal without turning that cleanup race into a CI failure.
if ((status != 0)) && [[ "$output" != *"hook capture and input smoke test passed"* ]]; then
    exit "$status"
fi
wait "$recovery_pid"
recovery_pid=""

(
    cd "$project_root/build/hook"
    WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES=wevtapi=n \
        timeout 20s xvfb-run -a wine ./orientation-selftest.exe
)

(
    cd "$project_root/build/hook"
    WINEPREFIX="$prefix" python3 "$project_root/tests/input-forwarding.py"
)
