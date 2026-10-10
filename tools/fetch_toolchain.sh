#!/bin/sh
# What preparing the game needs on the player's machine, so that it needs nothing there:
# zig (its `zig cc`, clang with every libc it targets) and a standalone Python for
# recomp/recomp.py. Pinned versions, checked against their SHA-256, unpacked into <dest>:
#
#   <dest>/zig/zig[.exe]                 and zig's lib/
#   <dest>/python/bin/python3            (python/python.exe on Windows)
#   <dest>/python-arm64, python-x86_64   instead, for a universal macOS app (-u): one zig
#                                        binary for both, one Python each
#
#   tools/fetch_toolchain.sh <dest> [-u]
#
# runtime/prepare.cpp finds them in an app's resources/toolchain, or where GCN_TOOLCHAIN says.
# Downloads are kept in <dest>/../toolchain-downloads, so a second run is offline.
set -eu
dest=$1
universal=${2:-}

ZIG=0.16.0
PY=3.12.14
PBS=20260924

case "$(uname -s)" in
    Darwin) os=macos ;;
    Linux) os=linux ;;
    MINGW*|MSYS*|CYGWIN*) os=windows ;;
    *) echo "fetch_toolchain: unknown system $(uname -s)" >&2; exit 1 ;;
esac
arch=$(uname -m)
if [ "$os" = windows ]; then
    case "${PROCESSOR_ARCHITECTURE:-AMD64}" in ARM64) arch=aarch64 ;; *) arch=x86_64 ;; esac
fi
case "$arch" in arm64|aarch64) arch=aarch64 ;; x86_64|amd64) arch=x86_64 ;; esac

zig_sha() {
    case "$1" in
        x86_64-windows) echo 68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e ;;
        aarch64-windows) echo aee38316ee4111717900f45dd3130145c39289e105541d737eb8c5ed653c78ef ;;
        x86_64-linux) echo 70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00 ;;
        aarch64-linux) echo ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17 ;;
        x86_64-macos) echo 0387557ed1877bc6a2e1802c8391953baddba76081876301c522f52977b52ba7 ;;
        aarch64-macos) echo b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489 ;;
    esac
}
py_triple() {
    case "$1" in
        x86_64-windows) echo x86_64-pc-windows-msvc ;;
        aarch64-windows) echo aarch64-pc-windows-msvc ;;
        x86_64-linux) echo x86_64-unknown-linux-gnu ;;
        aarch64-linux) echo aarch64-unknown-linux-gnu ;;
        x86_64-macos) echo x86_64-apple-darwin ;;
        aarch64-macos) echo aarch64-apple-darwin ;;
    esac
}
py_sha() {
    case "$1" in
        x86_64-windows) echo c5bf8edfe858c1df9891be498b5bbc8761d383df5b9790658b088fea4870433a ;;
        aarch64-windows) echo 1d07bd9c97e6e1942b290bd2daa22d5a41e1e5acd2fd3d4c696892ceffe47144 ;;
        x86_64-linux) echo 269b2c99e4db15b242bf01832f4fea1e8f1a664f273cff519393f296e9820b41 ;;
        aarch64-linux) echo c8499b61252c433280f134df954464d19811527b31cb920c35fc6967c1222e35 ;;
        x86_64-macos) echo 7ea9761b9069c10b9a20531d568645849d604c59e9c7f11f6659f1e1790c968e ;;
        aarch64-macos) echo c2edb321cd32ec2b170df208db0446dccc4398db602ca27cf2079098fb1f7d9d ;;
    esac
}

mkdir -p "$dest"
dest=$(cd "$dest" && pwd)
cache="$dest/../toolchain-downloads"
mkdir -p "$cache"

# fetch <url> <sha256>: the file in the cache, downloaded if it is not there, and checked.
fetch() {
    f="$cache/$(basename "$1")"
    if [ ! -f "$f" ]; then
        curl -fsSL --retry 3 -o "$f.part" "$1"
        mv "$f.part" "$f"
    fi
    got=$( (shasum -a 256 "$f" 2>/dev/null || sha256sum "$f") | cut -d' ' -f1)
    if [ "$got" != "$2" ]; then
        echo "fetch_toolchain: $(basename "$1"): SHA-256 $got, expected $2" >&2
        rm -f "$f"
        exit 1
    fi
    echo "$f"
}

unpack() {   # <archive> <dir>: its single top directory's contents into <dir>
    rm -rf "$2" "$2.tmp" && mkdir -p "$2.tmp"
    case "$1" in
        *.zip) (cd "$2.tmp" && unzip -q "$1") ;;
        *.tar.xz) tar -xJf "$1" -C "$2.tmp" ;;
        *) tar -xzf "$1" -C "$2.tmp" ;;
    esac
    mv "$2.tmp"/* "$2" && rmdir "$2.tmp"
}

zig_one() {   # <arch> <dir>
    p=$1-$os
    ext=tar.xz; [ "$os" = windows ] && ext=zip
    a=$(fetch "https://ziglang.org/download/$ZIG/zig-$p-$ZIG.$ext" "$(zig_sha "$p")")
    unpack "$a" "$2"
}
py_one() {   # <arch> <dir>
    p=$1-$os
    a=$(fetch "https://github.com/astral-sh/python-build-standalone/releases/download/$PBS/cpython-$PY+$PBS-$(py_triple "$p")-install_only_stripped.tar.gz" "$(py_sha "$p")")
    unpack "$a" "$2"
}

if [ "$universal" = -u ] && [ "$os" = macos ]; then
    zig_one aarch64 "$dest/zig"
    zig_one x86_64 "$dest/zig-x86_64"
    lipo -create "$dest/zig/zig" "$dest/zig-x86_64/zig" -output "$dest/zig/zig.universal"
    mv "$dest/zig/zig.universal" "$dest/zig/zig"
    rm -rf "$dest/zig-x86_64"
    py_one aarch64 "$dest/python-arm64"
    py_one x86_64 "$dest/python-x86_64"
else
    zig_one "$arch" "$dest/zig"
    py_one "$arch" "$dest/python"
fi
# Only what building C for this system needs: zig carries every libc it can target (170 MB
# of headers), its C++ libraries and its documentation; Python, its test suite.
libc="$dest/zig/lib/libc"
case $os in
    macos) keep='^(any-darwin-any|darwin)$' ;;
    windows) keep='^(any-windows-any|mingw)$' ;;
    linux) keep="^($arch-linux-gnu|$arch-linux-any|x86-linux-any|any-linux-any|generic-glibc|glibc)\$" ;;
esac
for d in "$libc"/* "$libc"/include/*; do
    [ -d "$d" ] && [ "$(basename "$d")" != include ] && ! basename "$d" | grep -Eq "$keep" && rm -rf "$d"
done
rm -rf "$dest/zig/doc" "$dest/zig/lib/libcxx" "$dest/zig/lib/libcxxabi" "$dest/zig/lib/libtsan" "$dest/zig/lib/docs" \
       "$dest"/python*/include "$dest"/python*/share "$dest"/python*/tcl "$dest"/python*/lib/*tcl* "$dest"/python*/lib/*tk*
for py in "$dest"/python*/lib/python3.* "$dest"/python*/Lib; do
    [ -d "$py" ] && (cd "$py" && rm -rf test idlelib tkinter turtledemo ensurepip lib2to3 pydoc_data site-packages/pip*)
done
echo "toolchain: zig $ZIG and Python $PY in $dest"
