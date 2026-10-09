#!/usr/bin/env bash
# Install uur into ~/.local: binary, hook DLLs, icon and an application
# menu entry.  Clicking "UU Remote" in the launcher then runs the whole
# stack; no PATH edits are needed (the desktop entry uses absolute paths).
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
readonly root
prefix="${UUR_INSTALL_PREFIX:-$HOME/.local}"

[[ -x "$root/target/release/uur" ]] || {
    printf 'missing %s — run: cargo build --release\n' \
        "$root/target/release/uur" >&2
    exit 1
}
[[ -f "$root/build/hook/wevtapi.dll" && -f "$root/build/hook/uur-hook.dll" ]] || {
    printf 'missing hook DLLs — run: ./hook/build.sh\n' >&2
    exit 1
}
[[ -f "$root/capture/uur-pw-capture" ]] || {
    printf 'missing capture helper — run: ./capture/build.sh\n' >&2
    exit 1
}

install -Dm0755 "$root/target/release/uur" "$prefix/bin/uur"
for helper in uur-pw-capture; do
    install -Dm0755 "$root/capture/$helper" "$prefix/lib/uur/$helper"
done
for dll in wevtapi.dll uur-hook.dll wtsapi32.dll winlogon.exe \
    uur-terminal-proxy.exe uur-mux-proxy.exe uur-launch-proxy.exe; do
    install -Dm0644 "$root/build/hook/$dll" "$prefix/lib/uur/hook/$dll"
done

# Menu entry: absolute Exec path so the user's PATH is irrelevant.
install -Dm0644 "$root/packaging/uur.desktop" \
    "$prefix/share/applications/uur.desktop"
sed -i "s|^Exec=.*|Exec=$prefix/bin/uur run|; s|^TryExec=.*|TryExec=$prefix/bin/uur|" \
    "$prefix/share/applications/uur.desktop"
if [[ -f "$root/packaging/uur-icon.png" ]]; then
    install -Dm0644 "$root/packaging/uur-icon.png" \
        "$prefix/share/icons/hicolor/256x256/apps/uur.png"
fi

printf 'installed:\n  %s/bin/uur\n  %s/lib/uur/\n  %s/share/applications/uur.desktop\n' \
    "$prefix" "$prefix" "$prefix"
UUR_COMMAND="$prefix/bin/uur" "$root/packaging/post-install-message"
