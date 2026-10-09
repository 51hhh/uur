#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/uur-check.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT

cargo fmt --manifest-path "$project_root/Cargo.toml" --check
cargo clippy --manifest-path "$project_root/Cargo.toml" \
    --all-targets -- -D warnings
cargo test --manifest-path "$project_root/Cargo.toml" --all-targets
cc -std=c11 -O2 -Wall -Wextra -Werror \
    "$project_root/tests/frame-protocol.c" -o "$temporary/frame-protocol"
"$temporary/frame-protocol"
cc -std=c11 -O2 -Wall -Wextra -Werror \
    "$project_root/tests/frame-copy.c" -o "$temporary/frame-copy"
"$temporary/frame-copy"
cc -std=c11 -O2 -Wall -Wextra -Werror \
    "$project_root/tests/cursor-overlay.c" -o "$temporary/cursor-overlay"
"$temporary/cursor-overlay"
cc -std=c11 -O2 -Wall -Wextra -Werror \
    "$project_root/tests/capture-mapping.c" -o "$temporary/capture-mapping"
"$temporary/capture-mapping"
cc -std=c11 -O2 -Wall -Wextra -Werror \
    "$project_root/tests/capture-layout.c" -o "$temporary/capture-layout"
"$temporary/capture-layout"
cc -std=c11 -O2 -Wall -Wextra -Werror \
    "$project_root/tests/input-mapping.c" -o "$temporary/input-mapping"
"$temporary/input-mapping"
"$project_root/hook/build.sh"
"$project_root/capture/build.sh"
