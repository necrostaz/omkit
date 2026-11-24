#!/bin/sh
# Run the host tests built by `make -C OMKit test` (OMKit/Makefile builds the programs; run from OMKit/).
#
#   sh tests/host/run.sh <dir of the test programs> <host C compiler> [<module dir> <MN10300 prefix> <python>]
#
# <module dir> holds HELLO.OMM built with the MN10300 toolchain and <MN10300 prefix> is its tool prefix
# (.../bin/mn10300-elf-); without them (empty) the module loader and the firmware symbol checks are skipped.
# Each program runs in a fresh temporary folder (it stands for the card); prints one line per check and exits 1 when
# any check failed.
set -u
BIN=$1
HOST_CC=$2
MODS=${3:-}
TC=${4:-}
PYTHON=${5:-python3}

failed=0
pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; failed=1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

# core parts (tests/host/test_core.c): the default group, then cfg, mem and power, each on its own empty "card"
for group in all cfg mem power; do
    mkdir "$tmp/$group"
    arg=$group
    [ "$group" = all ] && arg=
    # shellcheck disable=SC2086  # no argument for the default group
    out=$("$BIN/test_core" "$tmp/$group" $arg 2>&1)
    if [ "$out" = ok ]; then pass "core: $group"; else fail "core: $group"; echo "$out" | sed 's/^/        /'; fi
done

# The test scenario (tests/host/test_shoot.c): its state machine against a simulated camera, by keys and by wifi events
for path in keys wifi; do
    mkdir "$tmp/shoot_$path"
    out=$("$BIN/test_shoot" "$tmp/shoot_$path" $path 2>&1)
    if [ "$out" = ok ]; then pass "core: test scenario ($path)"; else fail "core: test scenario ($path)"; echo "$out" | sed 's/^/        /'; fi
done

# A module sees only src/sdk/include: the public header declares no driver, and the driver header is not there
if printf '#include "omk_api.h"\n#ifdef OMK_DRV_H\n#error driver\n#endif\nint f(const omk_api *a) { return a->version; }\n' |
        "$HOST_CC" -x c -fsyntax-only -Werror -Isrc/sdk/include - && [ ! -e src/sdk/include/omk_drv.h ]; then
    pass "sdk: no driver in the module API"
else
    fail "sdk: no driver in the module API"
fi

# The debug service (tests/host/test_debug.c): the RAM ring over power-ons (a torn end, wrap, a damaged header), the
# memory the computer may read, the USB operations and their jobs, no card writes; each on its own empty "card"
for group in ring memread proto write nocard; do
    mkdir "$tmp/debug_$group"
    out=$("$BIN/test_debug" "$tmp/debug_$group" $group 2>&1)
    if [ "$out" = ok ]; then pass "debug: $group"; else fail "debug: $group"; echo "$out" | sed 's/^/        /'; fi
done

# The black-and-white rule of an ISP block (omk_ispb_mono) and FILMLOOK's use of it after its filter (edits_mono):
# a line is the parts present, Cb, Cr, the nine matrix numbers, the filter 0..8 and its level; the answer is
# "omk_ispb_mono edits_mono". Present bits: 0x1f all, 0x1 the matrix, 0x8 the gains.
cat > "$tmp/ispb.in" <<'EOF'
31 0 0 19 62 47 19 62 47 19 62 47 0 0
31 144 182 25 57 46 24 58 46 24 57 47 0 0
31 136 172 80 41 7 5 101 22 7 13 108 0 0
31 136 172 40 60 28 42 58 28 40 60 28 0 0
31 136 172 40 60 28 43 57 28 40 60 28 0 0
23 0 0 80 41 7 5 101 22 7 13 108 0 0
31 0 172 80 41 7 5 101 22 7 13 108 0 0
30 136 172 19 62 47 19 62 47 19 62 47 0 0
0 0 0 0 0 0 0 0 0 0 0 0 0 0
31 0 0 19 62 47 19 62 47 19 62 47 3 2
31 144 182 25 57 46 24 58 46 24 57 47 1 2
30 136 172 0 0 0 0 0 0 0 0 0 8 3
EOF
# Expected, line by line: equal-row matrix with zero gains, near-equal rows (within 2), a colour matrix, rows 2 and 3
# apart (2/3 threshold), gains absent so the matrix decides, Cb 0 with gains given, no matrix (the base mode's colour),
# nothing present, a black-and-white filter keeps zero-gain mono, a filter's unequal rows make a row-equal mono a colour
# one, a filter sets the matrix bit with unequal rows
cat > "$tmp/ispb.want" <<'EOF'
1 1
1 1
0 0
1 1
0 0
0 0
1 1
0 0
0 0
1 1
1 0
0 0
EOF
if "$BIN/test_ispb" < "$tmp/ispb.in" | sed 's/ *$//' > "$tmp/ispb.out" && cmp -s "$tmp/ispb.out" "$tmp/ispb.want"; then
    pass "ispb: black-and-white rule"
else
    fail "ispb: black-and-white rule"
    diff "$tmp/ispb.want" "$tmp/ispb.out" | sed 's/^/        /'
fi

# FILMLOOK's .isp file code (tests/host/test_ispfile.c): the table CRC-32 against the bit-wise one, and the bundled
# presets through the file checks
if out=$("$BIN/test_ispfile" resources/modules/filmlook/presets/*.ISP 2>&1) && [ "$out" = ok ]; then
    pass "filmlook: .isp file checks and CRC-32 ($(ls resources/modules/filmlook/presets/*.ISP | wc -l) bundled presets)"
else
    fail "filmlook: .isp file checks and CRC-32"
    echo "$out" | sed 's/^/        /'
fi

# FILMLOOK's start (tests/host/test_filmlook.c): the module's own code against a stand-in for core, with the bundled
# presets. "first" leaves the look on with an edited preset in the store kept in the folder; the others are later
# power-ons from that store
mkdir "$tmp/filmlook"
for what in first again leftlook notnatural gone notlisted format2; do
    out=$("$BIN/test_filmlook" "$tmp/filmlook" resources/modules/filmlook/presets "$what" 2>&1)
    if [ "$out" = ok ]; then pass "filmlook start: $what"; else fail "filmlook start: $what"; echo "$out" | sed 's/^/        /'; fi
done

# FILMLOOK's tables under the editors (the same program, an empty "card"): every preset with a row of curve edits -
# each block byte for byte the file's with the edits - and the pacing of quick edits
mkdir "$tmp/filmedits"
out=$("$BIN/test_filmlook" "$tmp/filmedits" resources/modules/filmlook/presets edits 2>&1)
if [ "$out" = ok ]; then pass "filmlook edits: the blocks and the pace"; else fail "filmlook edits"; echo "$out" | sed 's/^/        /'; fi

# The driver's plain C parts (src/platform/om5m2/fw1100): the grain texture's fill by rows against the word-by-word
# one and the definition; the editors' quarter sine table against the full one it replaced; the column's switches
for t in test_texfill test_edtrig test_rowsw; do
    if out=$("$BIN/$t" 2>&1) && [ "$out" = ok ]; then pass "driver: ${t#test_}"; else fail "driver: ${t#test_}"; echo "$out" | sed 's/^/        /'; fi
done

if [ -z "$MODS" ] || [ -z "$TC" ]; then
    echo "  skip  module loader and firmware symbol checks: no MN10300 toolchain (docs/build.md)"
    exit $failed
fi

# The module loader with a real HELLO.OMM (tests/host/test_mod.c): relocation, API, refusals, unloading
for what in load fail power; do
    mkdir "$tmp/mod_$what"
    out=$("$BIN/test_mod" "$tmp/mod_$what" "$MODS/HELLO.OMM" "$what" 2>&1)
    if [ "$out" = ok ]; then pass "loader: $what"; else fail "loader: $what"; echo "$out" | sed 's/^/        /'; fi
done

# A module cannot refer to the firmware: the link fails, and when forced, mkmod refuses the undefined symbol
echo 'void fw(void); int module_init(void) { fw(); return 0; }' > "$tmp/bad.c"
flags="-Os -ffreestanding -nostdlib -nostartfiles -Isrc/sdk/include -Wl,-q -Wl,-T,src/modules/module.ld"
# shellcheck disable=SC2086
if "${TC}gcc" $flags "$tmp/bad.c" -o "$tmp/a.elf" 2>/dev/null; then
    fail "module: a firmware symbol is not linked"
else
    pass "module: a firmware symbol is not linked"
fi
rc=0
# shellcheck disable=SC2086
"${TC}gcc" $flags -Wl,--unresolved-symbols=ignore-all "$tmp/bad.c" -o "$tmp/b.elf" > "$tmp/mkmod.err" 2>&1 &&
    { $PYTHON tools/mkmod.py build "$tmp/b.elf" "$tmp/B.OMM" --name BAD > /dev/null 2> "$tmp/mkmod.err" || rc=$?; }
if [ "$rc" = 1 ] && grep -q _fw "$tmp/mkmod.err"; then
    pass "module: mkmod refuses an undefined symbol"
else
    fail "module: mkmod refuses an undefined symbol"
    sed 's/^/        /' "$tmp/mkmod.err" 2>/dev/null
fi
exit $failed
