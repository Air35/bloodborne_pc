#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# sotc: one self-extracting file with the built port for a machine with no build environment
# (sotc-remake ADR-0022): dist/SotC-Linux-x86_64-<version>.run, and with `debug` also
# dist/SotC-Linux-x86_64-debug-<version>.run (adds gdb, objdump and the Khronos validation
# layer, unstripped binaries).
#
# Run inside the build sandbox, after build.sh:
#   tools/sotc-sandbox.sh --build -- bash packaging/sotc-bundle.sh [release|debug] [version]
#
# What goes in: bb-probe and libbbgpu.so, the preparation scripts, sotc.py, and the shared
# libraries they need that are not part of the host's base system, graphics driver or audio
# stack (those must be the host's own: Mesa/RADV, the Vulkan loader, glibc, libstdc++, X11,
# Wayland, ALSA/Pulse/PipeWire). Every bundled ELF gets RUNPATH $ORIGIN-relative, so nothing
# depends on LD_LIBRARY_PATH. Never anything of the game, firmware or user data: the script
# refuses .sprx/.prx/.rif/.idx/param.sfo/eboot in the staging tree.
#
# The file is a POSIX shell header plus a gzip tarball: `./SotC...run --extract <dir>` (default
# $XDG_DATA_HOME/sotc-remake/app/<version>) prints the folder; the game then runs through
# tools/sotc-sandbox.sh --run (tools/sotc-amd-test.sh does all of it).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
kind=${1:-release}
version=${2:-${SOTC_BUNDLE_VERSION:-dev}}
case $kind in release|debug) ;; *) echo "usage: $0 [release|debug] [version]" >&2; exit 2 ;; esac
[[ -f out/bb-probe && -f out/gpu/libbbgpu.so && -f out/bb-gpu-capabilities ]] ||
    { echo 'Build first: bash build.sh' >&2; exit 1; }
for tool in patchelf ldd strip gzip tar sha256sum; do
    command -v "$tool" >/dev/null || { echo "Need $tool" >&2; exit 1; }
done

suffix=; [[ $kind == debug ]] && suffix=-debug
name=SotC-Linux-x86_64$suffix-$version
stage=out/bundle/$name
root=$stage/sotc
rm -rf -- "$stage"
mkdir -p "$root/bin/gpu" "$root/lib" "$root/scripts" "$root/tests"

# The port: binaries where sotc.py looks (bin/), the GPU library next to them ($ORIGIN/gpu).
install -m755 out/bb-probe out/bb-gpu-capabilities "$root/bin/"
install -m755 out/gpu/libbbgpu.so "$root/bin/gpu/"
install -m644 sotc.py LICENSE "$root/"
install -m644 scripts/*.py "$root/scripts/"
[[ -d patches ]] && cp -r patches "$root/patches"
cp tests/paths.py "$root/tests/" 2>/dev/null || true

# Debug extras: the host tools, their libraries come with the scan below.
if [[ $kind == debug ]]; then
    install -m755 "$(command -v gdb)" "$(command -v objdump)" "$root/bin/"
    [[ -d /usr/share/gdb ]] && mkdir -p "$root/share" && cp -r /usr/share/gdb "$root/share/gdb"
    layer=/usr/lib/x86_64-linux-gnu/libVkLayer_khronos_validation.so
    json=/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json
    if [[ -f $layer && -f $json ]]; then
        install -m644 "$layer" "$root/lib/"
        mkdir -p "$root/share/vulkan/explicit_layer.d"
        # The loader resolves a relative library_path against the manifest's folder.
        sed 's#"library_path": *"[^"]*"#"library_path": "../../../lib/libVkLayer_khronos_validation.so"#' \
            "$json" > "$root/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
    fi
else
    install -m755 "$(command -v objdump)" "$root/bin/"
fi

# Host-provided libraries (never bundled): libc family, driver and window/audio stack.
host='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl|libnsl|libgcc_s|libstdc\+\+|libvulkan|libGL.*|libEGL|libGLX|libGLdispatch|libOpenGL|libgbm|libdrm.*|libva.*|libvdpau|libOpenCL|libX.*|libxcb.*|libxkbcommon.*|libwayland-.*|libdecor-.*|libasound|libpulse.*|libpipewire-.*|libjack|libdbus-1|libsystemd|libudev|libselinux|libmount|libblkid|libcap|libexpat|libz|libfontconfig|libfreetype|libharfbuzz|libglib-2\.0|libgobject-2\.0|libgio-2\.0|libgmodule-2\.0|libffi|libuuid|libcom_err|libkeyutils|libgssapi_krb5|libkrb5.*|libk5crypto)\.so'
bundle_deps() { # ELF files... -> copies their non-host dependencies into lib/, repeatedly
    local changed=1
    while ((changed)); do
        changed=0
        while read -r lib path; do
            [[ -z $path || $lib =~ $host ]] && continue
            [[ -e $root/lib/$lib ]] && continue
            install -m644 "$path" "$root/lib/$lib"
            changed=1
        done < <(for elf in "$@" "$root"/lib/*.so*; do [[ -f $elf ]] && ldd "$elf" 2>/dev/null; done |
                 awk '/=> \//{print $1, $3}' | sort -u)
    done
}
mapfile -t elves < <(find "$root/bin" -type f -perm -u+x; echo "$root/bin/gpu/libbbgpu.so")
bundle_deps "${elves[@]}"

# RUNPATHs: binaries look in ../lib (and gpu/), libraries next to themselves.
# shellcheck disable=SC2016 # $ORIGIN is for the dynamic loader, not the shell
for elf in "$root"/bin/*; do
    [[ -f $elf ]] && patchelf --set-rpath '$ORIGIN/gpu:$ORIGIN/../lib' "$elf"
done
patchelf --set-rpath '$ORIGIN:$ORIGIN/../../lib' "$root/bin/gpu/libbbgpu.so"
for lib in "$root"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$lib"; done
if [[ $kind == release ]]; then
    strip --strip-debug "$root"/bin/bb-probe "$root"/bin/gpu/libbbgpu.so
fi

# The entry: the sandbox runs it with the port folder as the current directory.
cat > "$root/sotc-run" <<'EOF'
#!/usr/bin/env bash
# sotc: starts the game from an extracted bundle (inside tools/sotc-sandbox.sh --run).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
export PATH="$here/bin:$PATH"
if [[ -d $here/share/vulkan/explicit_layer.d ]]; then
    export VK_ADD_LAYER_PATH="$here/share/vulkan/explicit_layer.d${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
fi
export SOTC_OUT_DIR=${SOTC_OUT_DIR:-$HOME/sotc-out} SOTC_USER_DIR=${SOTC_USER_DIR:-$HOME/sotc-user}
exec python3 -I "$here/sotc.py" "$@"
EOF
chmod 755 "$root/sotc-run"
if [[ $kind == debug ]]; then
    cat > "$root/sotc-gdb" <<'EOF'
#!/usr/bin/env bash
# sotc: the game under the bundled gdb, batch mode, backtrace of every thread on a stop.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
export SOTC_WRAP="[\"$here/bin/gdb\", \"-q\", \"-batch\", \"--data-directory=$here/share/gdb\", \"-ex\", \"handle SIGSEGV nostop noprint pass\", \"-ex\", \"handle SIGUSR1 nostop noprint pass\", \"-ex\", \"handle SIGUSR2 nostop noprint pass\", \"-ex\", \"run\", \"-ex\", \"thread apply all bt 25\", \"--args\"]"
exec "$here/sotc-run" "$@"
EOF
    chmod 755 "$root/sotc-gdb"
fi

# Nothing of the game, firmware, licenses or user data, whatever the build tree held.
if find "$stage" \( -iname '*.sprx' -o -iname '*.prx' -o -iname '*.rif' -o -iname '*.idx' \
        -o -iname 'param.sfo' -o -iname 'eboot.bin' -o -iname '*.psarc' -o -iname '*.pup' \) | grep -q .; then
    echo 'REFUSED: game or firmware files in the bundle' >&2; exit 1
fi
(cd "$root" && find . -type f ! -name MANIFEST.sha256 -print0 | sort -z | xargs -0 sha256sum) > "$root/MANIFEST.sha256"

mkdir -p dist
payload=$stage/payload.tar.gz
tar -C "$stage" --owner=0 --group=0 --numeric-owner -czf "$payload" sotc
cat > "$stage/header.sh" <<EOF
#!/bin/sh
# Shadow of the Colossus Linux port (sotc-remake), $kind bundle $version. Self-extracting:
#   $name.run --extract [dir]   unpacks (default \$XDG_DATA_HOME/sotc-remake/app/$name)
# and prints the folder. Run the game with tools/sotc-amd-test.sh or tools/sotc-sandbox.sh --run.
set -eu
dest=\${2:-\${XDG_DATA_HOME:-\$HOME/.local/share}/sotc-remake/app/$name}
case "\${1:-}" in
--extract) ;;
*) echo "usage: \$0 --extract [dir]   (then run it through tools/sotc-amd-test.sh)" >&2; exit 2 ;;
esac
if [ ! -f "\$dest/sotc/.complete" ]; then
    mkdir -p "\$dest"
    line=\$(awk '/^__PAYLOAD_BELOW__\$/ { print NR + 1; exit 0 }' "\$0")
    tail -n +"\$line" "\$0" | tar -xz -C "\$dest"
    (cd "\$dest/sotc" && sha256sum -c --quiet MANIFEST.sha256)
    touch "\$dest/sotc/.complete"
fi
echo "\$dest/sotc"
exit 0
__PAYLOAD_BELOW__
EOF
cat "$stage/header.sh" "$payload" > "dist/$name.run"
chmod 755 "dist/$name.run"
sha256sum "dist/$name.run" > "dist/$name.run.sha256"
echo "Bundle: dist/$name.run ($(du -h "dist/$name.run" | cut -f1)); $(ls "$root/lib" | wc -l) bundled libraries"
