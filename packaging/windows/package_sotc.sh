#!/usr/bin/env bash
# sotc: builds dist/SotC-PC/ and dist/SotC-PC.zip: bb-probe.exe with the MSYS2 CLANG64 DLLs it
# needs, the preparation scripts, sotc.py and SotC.bat. Players need a Windows Python 3 and their
# own game dump; sound needs their own console's libSceNgs2/libSceUlt (never packaged here).
# Run from an MSYS2 CLANG64 shell after `bash build.sh`.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
export MSYS2_ROOT=${MSYS2_ROOT:-/i/msys64}
export PATH="$MSYS2_ROOT/clang64/bin:$MSYS2_ROOT/usr/bin:$PATH"
[[ -f out/bb-probe.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

dest=out/stage/SotC-PC
rm -rf -- out/stage
mkdir -p "$dest/bin" "$dest/scripts"
# Without debug information (out/ keeps the symbols for crash reports).
llvm-strip --strip-debug -o "$dest/bin/bb-probe.exe" out/bb-probe.exe
# Every DLL the loader takes from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/bb-probe.exe" | awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
    cp -u "$dll" "$dest/bin/"
done
cp scripts/prepare.py scripts/link_libc.py scripts/link_modules.py scripts/content_profile.py \
   scripts/sotc_cpu.py scripts/sotc_modes.py "$dest/scripts/"
cp sotc.py SotC.bat LICENSE "$dest/"
cp docs/sotc/README-SotC.md "$dest/README.md"
cat > "$dest/sotc.ini" <<'EOF'
# Shadow of the Colossus PC - settings (see README.md)
# Folder of your game dump (the one with eboot.bin):
SOTC_GAME_DIR=..\CUSA08809
# Folder with libSceNgs2.sprx and libSceUlt.sprx from YOUR OWN console (for sound):
# SOTC_SYS_MODULES=C:\path\to\sys_modules
# Optional modes: af16 (sharp textures), fps60 (no 30 FPS cap, experimental):
# SOTC_MODES=af16
EOF
# Sony firmware must never be in the package.
if find "$dest" -iname '*.sprx' -o -iname '*.prx' | grep -q .; then
    echo 'refusing to package firmware modules' >&2; exit 1
fi

mkdir -p dist
rm -rf dist/SotC-PC.zip
python -c 'import shutil, sys; shutil.make_archive("dist/SotC-PC", "zip", "out/stage", "SotC-PC")'
# The playable copy keeps its settings and saves.
mkdir -p dist/SotC-PC
cp -r "$dest/bin" "$dest/scripts" dist/SotC-PC/
cp "$dest/sotc.py" "$dest/SotC.bat" "$dest/README.md" "$dest/LICENSE" dist/SotC-PC/
[[ -f dist/SotC-PC/sotc.ini ]] || cp "$dest/sotc.ini" dist/SotC-PC/
echo "dist/SotC-PC.zip ($(du -h dist/SotC-PC.zip | cut -f1)) and dist/SotC-PC/"
