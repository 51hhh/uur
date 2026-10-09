#!/usr/bin/env bash
# Cross-build the Windows PE hook components (needs the mingw-w64
# toolchain; see packaging/aur/PKGBUILD makedepends).
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
readonly root
out="$root/build/hook"
if [[ -n ${UU_MINGW_CC:-} ]]; then
    cc=$UU_MINGW_CC
elif command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    cc=x86_64-w64-mingw32-gcc
else
    cc=x86_64-w64-mingw32-cc
fi
strip="${UU_MINGW_STRIP:-x86_64-w64-mingw32-strip}"
dlltool="${UU_MINGW_DLLTOOL:-x86_64-w64-mingw32-dlltool}"
flags=(-std=c11 -O2 -Wall -Wextra -Werror -Wl,--no-insert-timestamp)

mkdir -p "$out"

for tool in "$cc" "$dlltool" "$strip"; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'missing tool: %s\n' "$tool" >&2; exit 1; }
done

"$dlltool" --def "$root/hook/uur-hook.def" \
    --output-lib "$out/libuur-hook.dll.a"

"$cc" "${flags[@]}" -shared -o "$out/uur-hook.dll" \
    "$root/hook/uur-hook.c" "$root/hook/uur-hook.def" \
    -lws2_32 -lgdi32 -luser32 -lkernel32

"$cc" "${flags[@]}" -shared -o "$out/wevtapi.dll" \
    "$root/hook/wevtapi.c" "$root/hook/wevtapi.def" \
    "$out/libuur-hook.dll.a"

"$cc" "${flags[@]}" -shared -o "$out/wtsapi32.dll" \
    "$root/hook/wtsapi32.c" -ladvapi32 -lkernel32

"$dlltool" --def "$root/hook/wevtapi.def" \
    --output-lib "$out/libwevtapi.dll.a"

"$cc" "${flags[@]}" -mwindows -o "$out/winlogon.exe" \
    "$root/hook/winlogon.c" -lkernel32

"$cc" "${flags[@]}" -mwindows -o "$out/uur-terminal-proxy.exe" \
    "$root/hook/terminal-proxy.c" -lws2_32 -lkernel32

"$cc" "${flags[@]}" -mwindows -o "$out/uur-mux-proxy.exe" \
    "$root/hook/mux-proxy.c" -lkernel32

"$cc" "${flags[@]}" -mwindows -o "$out/uur-launch-proxy.exe" \
    "$root/hook/launch-proxy.c" -lws2_32 -lkernel32

"$cc" "${flags[@]}" -o "$out/selftest.exe" \
    "$root/hook/selftest.c" -L"$out" -lwevtapi -luser32 -lgdi32
"$cc" "${flags[@]}" -o "$out/orientation-selftest.exe" \
    "$root/hook/orientation-selftest.c" -L"$out" -lwevtapi -luser32 -lgdi32

"$strip" --strip-unneeded "$out/uur-hook.dll" "$out/wevtapi.dll" \
    "$out/wtsapi32.dll" "$out/winlogon.exe" \
    "$out/uur-terminal-proxy.exe" "$out/uur-mux-proxy.exe" \
    "$out/uur-launch-proxy.exe"
printf 'built: %s\n' "$out"
