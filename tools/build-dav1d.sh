#!/usr/bin/env bash
# Mediathek - Builds the dav1d AV1 decoder as a static library, for the console or the PC.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/build-dav1d.sh ps5|host
#
# Prints the folder that holds include/dav1d/*.h and lib/libdav1d.a. PacBrew's FFmpeg has
# no AV1 decoder of its own, so the player feeds dav1d itself (VLC's PS5 port does the same).
# Meson is dav1d's only build system; it is pure Python and runs from its source release,
# so nothing is installed. nasm builds dav1d's x86 assembly, without which 4K cannot keep up.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-}
[[ $target == ps5 || $target == host ]] || {
    echo "usage: tools/build-dav1d.sh ps5|host" >&2
    exit 2
}

cache="$root/.deps/dav1d"
dav1d_version=1.5.4
dav1d_hash=686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5
dav1d_url="https://downloads.videolan.org/pub/videolan/dav1d/$dav1d_version/dav1d-$dav1d_version.tar.xz"
meson_version=1.6.1
meson_hash=1eca49eb6c26d58bbee67fd3337d8ef557c0804e30a6d16bfdf269db997464de
meson_url="https://github.com/mesonbuild/meson/releases/download/$meson_version/meson-$meson_version.tar.gz"

for command in python3 ninja nasm sha256sum tar wget; do
    command -v "$command" >/dev/null || {
        echo "missing required command for dav1d: $command (Debian/Ubuntu: apt install $command)" >&2
        exit 2
    }
done

# Downloads `url` to `file` unless a copy with `hash` is there already.
fetch() {
    local url=$1 file=$2 hash=$3
    if [[ -f $file ]] && printf '%s  %s\n' "$hash" "$file" | sha256sum --check --strict >/dev/null 2>&1; then
        return
    fi
    echo "==> [dav1d] Downloading ${file##*/}" >&2
    wget -q "$url" -O "$file.download"
    printf '%s  %s\n' "$hash" "$file.download" | sha256sum --check --strict >/dev/null
    mv "$file.download" "$file"
}

# Unpacks `archive` into `folder` once per hash.
unpack() {
    local archive=$1 folder=$2 hash=$3
    if [[ -d $folder && -f $folder.unpacked && $(<"$folder.unpacked") == "$hash" ]]; then
        return
    fi
    rm -rf -- "$folder"
    tar -xf "$archive" -C "$cache"
    printf '%s\n' "$hash" >"$folder.unpacked"
}

mkdir -p "$cache"
fetch "$dav1d_url" "$cache/dav1d-$dav1d_version.tar.xz" "$dav1d_hash"
fetch "$meson_url" "$cache/meson-$meson_version.tar.gz" "$meson_hash"
unpack "$cache/dav1d-$dav1d_version.tar.xz" "$cache/dav1d-$dav1d_version" "$dav1d_hash"
unpack "$cache/meson-$meson_version.tar.gz" "$cache/meson-$meson_version" "$meson_hash"

source_dir="$cache/dav1d-$dav1d_version"
build_dir="$cache/build-$target"
prefix="$cache/$target"
options=(--buildtype=release -Ddefault_library=static -Denable_tools=false
    -Denable_tests=false -Denable_examples=false -Denable_docs=false -Dbitdepths=8,16
    -Denable_asm=true --prefix="$prefix" --libdir=lib)

setup=()
if [[ $target == ps5 ]]; then
    sdk="$root/.deps/native/ps5-payload-sdk"
    [[ -x $sdk/bin/prospero-clang ]] || {
        echo "the PS5 payload SDK is missing; run make deps first" >&2
        exit 2
    }
    # The SDK's own wrapper: the target, the sysroot and the flags PacBrew's ports, FFmpeg
    # among them, are built with, so dav1d links into the app as they do.
    cross="$cache/ps5-cross.ini"
    cat >"$cross.new" <<EOF
[binaries]
c = '$sdk/bin/prospero-clang'
ar = '$sdk/bin/prospero-ar'
strip = '$sdk/bin/prospero-strip'
nasm = '$(command -v nasm)'

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF
    mv "$cross.new" "$cross"
    setup+=(--cross-file="$cross")
    compiler="$sdk/bin/prospero-clang $($sdk/bin/prospero-llvm-config --version)"
else
    # The tests run under the sanitizers; dav1d is built without them, as a system
    # library would be.
    cc=$(command -v "${HOST_CC:-clang}")
    export CC=$cc
    compiler="$cc $("$cc" -dumpversion)"
fi

stamp="$prefix/.built"
wanted="dav1d $dav1d_version meson $meson_version $compiler ${options[*]}"
if [[ ! -f $prefix/lib/libdav1d.a || ! -f $stamp || $(<"$stamp") != "$wanted" ]]; then
    echo "==> [dav1d] Building dav1d $dav1d_version for $target" >&2
    rm -rf -- "$build_dir" "$prefix"
    python3 "$cache/meson-$meson_version/meson.py" setup "$build_dir" "$source_dir" \
        "${setup[@]}" "${options[@]}" >"$cache/setup-$target.log" 2>&1 || {
        tail -40 "$cache/setup-$target.log" >&2
        exit 1
    }
    ninja -C "$build_dir" >"$cache/build-$target.log" 2>&1 || {
        grep -E 'error|FAILED' -A6 "$cache/build-$target.log" | head -60 >&2
        exit 1
    }
    python3 "$cache/meson-$meson_version/meson.py" install -C "$build_dir" --quiet >/dev/null
    # BSD-2-Clause: the notice goes with every copy of the app (tools/build.sh).
    cp "$source_dir/COPYING" "$prefix/COPYING"
    printf '%s\n' "$wanted" >"$stamp"
fi
printf '%s\n' "$prefix"
