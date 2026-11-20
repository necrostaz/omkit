#!/usr/bin/env bash
# Build the cross toolchain OMKit needs: GNU binutils 2.45 and GCC 13.3.0 (C only, with libgcc) for mn10300-elf.
# Linux or WSL (Debian/Ubuntu tested); no root needed. See OMKit/docs/build.md.
#
#   bash OMKit/toolchain/build-toolchain.sh                         # into ~/opt/mn10300
#   PREFIX=/opt/mn10300 WORK=/tmp/mn10300-work bash OMKit/toolchain/build-toolchain.sh
#
# Variables (all optional):
#   PREFIX      where the toolchain is installed (default ~/opt/mn10300, where the OMKit Makefiles look for it;
#               another prefix: pass TC=<PREFIX>/bin/mn10300-elf- to make)
#   WORK        downloads, sources and build folders (default ~/build/mn10300-toolchain; about 3.5 GB when done,
#               removable afterwards)
#   JOBS        parallel make jobs (default: the number of CPUs)
#   GNU_MIRROR  where binutils and GCC are downloaded from (default https://ftp.gnu.org/gnu); the archives are checked
#               against the SHA-256 sums below whatever the source. An archive already in WORK is checked and reused.
#               GCC's libraries come from gcc.gnu.org (below); without network put their archives into
#               WORK/src/gcc-<version>/ as well
#   FORCE=1     build again even when PREFIX already holds this toolchain
#
# Re-runs are safe: a finished step is skipped (a stamp file in WORK), an interrupted one is redone from a clean build
# folder. GCC's own libraries (gmp, mpfr, mpc, isl) are fetched by GCC's contrib/download_prerequisites, which checks
# them against the SHA-512 sums shipped inside the (checked) GCC archive, and are built inside the GCC tree.
#
# The versions are pinned: OMKit's binaries are built and tested with exactly these. The mn10300 target has no LRA
# register allocator, so newer GCC branches may drop it.
set -euo pipefail

BINUTILS_VER=2.45
BINUTILS_SHA256=c50c0e7f9cb188980e2cc97e4537626b1672441815587f1eab69d2a1bfbef5d2   # binutils-2.45.tar.xz
GCC_VER=13.3.0
GCC_SHA256=0845e9621c9543a13f484e94584a49ffc0129970e9914624235fc1d061a0c083        # gcc-13.3.0.tar.xz
TARGET=mn10300-elf

PREFIX=${PREFIX:-$HOME/opt/mn10300}
WORK=${WORK:-$HOME/build/mn10300-toolchain}
GNU_MIRROR=${GNU_MIRROR:-https://ftp.gnu.org/gnu}
FORCE=${FORCE:-0}

say() { printf '==> %s\n' "$*"; }
die() { printf 'build-toolchain: error: %s\n' "$*" >&2; exit 1; }

# --- host tools ---------------------------------------------------------------------------------------------------

missing=()
for c in gcc g++ make tar xz bzip2 gzip; do
    command -v "$c" >/dev/null 2>&1 || missing+=("$c")
done
command -v wget >/dev/null 2>&1 || command -v curl >/dev/null 2>&1 || missing+=("wget or curl")
if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    missing+=("sha256sum or shasum")
fi
command -v sha512sum >/dev/null 2>&1 || command -v shasum >/dev/null 2>&1 || missing+=("sha512sum or shasum")
if [ ${#missing[@]} -gt 0 ]; then
    {
        echo "build-toolchain: missing host tools: ${missing[*]}"
        echo "  Debian/Ubuntu (and WSL): sudo apt install build-essential bzip2 xz-utils wget"
        echo "  macOS (not tested):      xcode-select --install; brew install wget xz"
    } >&2
    exit 1
fi
# A C and C++ compiler that can build a program (build-essential also brings the C library headers)
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
printf '#include <stdio.h>\nint main(void) { return 0; }\n' > "$probe/t.c"
printf '#include <string>\nint main() { return std::string("x").size() - 1; }\n' > "$probe/t.cc"
gcc "$probe/t.c" -o "$probe/t" >/dev/null 2>&1 || die "the host C compiler cannot build a program (Debian/Ubuntu: sudo apt install build-essential)"
g++ "$probe/t.cc" -o "$probe/t" >/dev/null 2>&1 || die "the host C++ compiler cannot build a program (Debian/Ubuntu: sudo apt install build-essential)"

if [ -z "${JOBS:-}" ]; then
    JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
fi

# --- already installed? -------------------------------------------------------------------------------------------

installed() {
    local as="$PREFIX/bin/$TARGET-as" cc="$PREFIX/bin/$TARGET-gcc"
    [ -x "$as" ] && [ -x "$cc" ] || return 1
    "$as" --version 2>/dev/null | head -1 | grep -q " $BINUTILS_VER\$" || return 1
    [ "$("$cc" -dumpversion 2>/dev/null)" = "$GCC_VER" ] || return 1
    [ -f "$("$cc" -print-libgcc-file-name)" ] || return 1
}

if [ "$FORCE" != 1 ] && installed; then
    say "$TARGET binutils $BINUTILS_VER and GCC $GCC_VER are already in $PREFIX (FORCE=1 builds again)"
    exit 0
fi

mkdir -p "$PREFIX" 2>/dev/null || die "cannot create PREFIX $PREFIX"
[ -w "$PREFIX" ] || die "PREFIX $PREFIX is not writable"
mkdir -p "$WORK/logs" || die "cannot create WORK $WORK"
WORK=$(cd "$WORK" && pwd)
PREFIX=$(cd "$PREFIX" && pwd)
export PATH="$PREFIX/bin:$PATH"
say "PREFIX $PREFIX, WORK $WORK, $JOBS jobs"

# Run a step in a subshell, its output in WORK/logs/<name>.log; on failure show the end of the log
run() {
    local name=$1; shift
    if ! ( "$@" ) > "$WORK/logs/$name.log" 2>&1; then
        tail -n 30 "$WORK/logs/$name.log" >&2
        die "step '$name' failed; the full log: $WORK/logs/$name.log"
    fi
}
in_dir() { cd "$1" && shift && "$@"; }

# --- sources ------------------------------------------------------------------------------------------------------

fetch() {   # fetch <url> <file> <sha256>: download into WORK unless there, check the sum
    local url=$1 file=$WORK/$2 want=$3 got
    if [ ! -f "$file" ]; then
        say "downloading $url"
        rm -f "$file.part"
        if command -v wget >/dev/null 2>&1; then
            wget -q -O "$file.part" "$url" || die "download failed: $url (another GNU_MIRROR, or put $2 into $WORK by hand)"
        else
            curl -fsSL -o "$file.part" "$url" || die "download failed: $url (another GNU_MIRROR, or put $2 into $WORK by hand)"
        fi
        got=$(sha256 "$file.part")
        [ "$got" = "$want" ] || die "$2 from $url: SHA-256 $got, expected $want (file kept as $file.part)"
        mv "$file.part" "$file"
    fi
    got=$(sha256 "$file")
    [ "$got" = "$want" ] || die "$file: SHA-256 $got, expected $want; delete it and run again"
}

unpack() {  # unpack <archive> <dir>: a fresh copy unless done before
    local archive=$WORK/$1 dir=$WORK/src/$2
    [ -f "$dir/.omkit-unpacked" ] && return 0
    say "unpacking $1"
    rm -rf "$dir"
    mkdir -p "$WORK/src"
    tar -xf "$archive" -C "$WORK/src" || die "cannot unpack $archive"
    [ -d "$dir" ] || die "$archive did not give $dir"
    touch "$dir/.omkit-unpacked"
}

fetch "$GNU_MIRROR/binutils/binutils-$BINUTILS_VER.tar.xz" "binutils-$BINUTILS_VER.tar.xz" "$BINUTILS_SHA256"
fetch "$GNU_MIRROR/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz" "gcc-$GCC_VER.tar.xz" "$GCC_SHA256"
unpack "binutils-$BINUTILS_VER.tar.xz" "binutils-$BINUTILS_VER"
unpack "gcc-$GCC_VER.tar.xz" "gcc-$GCC_VER"

GCC_SRC=$WORK/src/gcc-$GCC_VER
if [ ! -f "$GCC_SRC/.omkit-prerequisites" ]; then
    say "GCC prerequisites (gmp, mpfr, mpc, isl)"
    run gcc-prerequisites in_dir "$GCC_SRC" ./contrib/download_prerequisites
    touch "$GCC_SRC/.omkit-prerequisites"
fi

# --- build --------------------------------------------------------------------------------------------------------

stage() {   # stage <name> <source dir> <made file> "<make targets>" "<install targets>" <configure options...>
    local name=$1 src=$2 made=$PREFIX/$3 targets=$4 install=$5; shift 5
    local dir=$WORK/build-$name
    if [ "$FORCE" != 1 ] && [ -f "$dir/.omkit-installed" ] && [ -x "$made" ] &&
            [ "$(cat "$dir/.omkit-configured" 2>/dev/null)" = "$PREFIX" ]; then
        say "$name: installed before, skipped"
        return 0
    fi
    if [ "$FORCE" = 1 ] || [ "$(cat "$dir/.omkit-configured" 2>/dev/null)" != "$PREFIX" ]; then
        rm -rf "$dir"
        mkdir -p "$dir"
        say "$name: configure"
        run "$name-configure" in_dir "$dir" "$src/configure" "$@"
        echo "$PREFIX" > "$dir/.omkit-configured"
    fi
    rm -f "$dir/.omkit-installed"
    say "$name: build (log: $WORK/logs/$name-build.log)"
    # shellcheck disable=SC2086  # the targets are words
    run "$name-build" make -C "$dir" -j"$JOBS" $targets
    say "$name: install"
    # shellcheck disable=SC2086
    run "$name-install" make -C "$dir" $install
    touch "$dir/.omkit-installed"
}

start=$(date +%s)
stage binutils "$WORK/src/binutils-$BINUTILS_VER" "bin/$TARGET-ld" all install \
    --target=$TARGET --prefix="$PREFIX" --disable-nls --disable-werror --disable-gdb --disable-sim --disable-gprofng
stage gcc "$GCC_SRC" "bin/$TARGET-gcc" "all-gcc all-target-libgcc" "install-gcc install-target-libgcc" \
    --target=$TARGET --prefix="$PREFIX" --enable-languages=c --without-headers --with-newlib --disable-multilib \
    --disable-nls --disable-shared --disable-threads --disable-libssp --disable-libquadmath --disable-libgomp \
    --disable-libatomic --disable-decimal-float --disable-bootstrap

# --- check --------------------------------------------------------------------------------------------------------

installed || die "the toolchain in $PREFIX does not report binutils $BINUTILS_VER and GCC $GCC_VER"
printf 'int f(int a) { return a * 3; }\n' > "$probe/f.c"
"$PREFIX/bin/$TARGET-gcc" -Os -ffreestanding -nostdlib -c "$probe/f.c" -o "$probe/f.o" \
    || die "$PREFIX/bin/$TARGET-gcc cannot compile a test file"
"$PREFIX/bin/$TARGET-as" --version | head -1
"$PREFIX/bin/$TARGET-gcc" --version | head -1
say "done in $(( ($(date +%s) - start) / 60 )) min: $PREFIX"
if [ "$PREFIX" != "$HOME/opt/mn10300" ]; then
    say "not the default prefix: build OMKit with  make -C OMKit TC=$PREFIX/bin/$TARGET-"
fi
say "the work folder $WORK is no longer needed (rm -rf it to free the space)"
