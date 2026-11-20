# The .grn grain tile container, format 2

A `.GRN` file is one film grain "tile" for the grain of the film look's stills: a small square of grain values that the
camera repeats over the whole picture with a random shift for every still, at a strength the user chooses. Core reads
the containers from `OMKIT\GRAIN\` (8.3 names, at most 32, in the order of their file names); the look's owner (FILMLOOK)
lets the user pick a tile and a strength (Off, Low, Med, High) per preset. Reference reader and
checker: `tools/grainset.py` (`parse`, `check`).

## 1. Rules

- Integers are little-endian. CRC-32 is the common one (zlib `crc32`).
- The file is the 64-byte header followed by the tile's pixels; nothing after them.
- Version 2 is the only one core reads (version 1, never used on a camera, had a drawn icon: the camera now prints the
  name itself).

## 2. Header (64 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | magic `OMGR` |
| 4 | u16 | version, 2 |
| 6 | u16 | header size, 64 |
| 8 | u32 | file size: 64 + side² |
| 12 | u32 | CRC-32 of bytes 0..63 with this field taken as 0 |
| 16 | char[24] | the name shown: 1..23 printable ASCII characters (32..126), no space at either end, zero padded (byte 39 is 0) |
| 40 | u16 | side: 128..768, a multiple of 64 that divides 1536 (128, 192, 256, 384, 512, 768) |
| 42 | u16 | the mean of the pixels x 256, rounded |
| 44 | u8 | the largest pixel value, at most 63 |
| 45 | u8 | flags, 0 |
| 46 | u16 | the tile's gain: 1..1024 - the strength High unless the next field gives the strengths |
| 48 | u16[3] | the gains of Low, Med, High: all 0 - the defaults; otherwise each 1..2047, not falling |
| 54 | u16 | 0 |
| 56 | u32 | CRC-32 of the pixels |
| 60 | u32 | 0 |

The default gains are integer: Low = (gain x 25 + 50) / 100, Med = (gain x 55 + 50) / 100, High = gain. A gain is the
blend gain of the grain (1024 adds the tile's deviation from its mean one to one to the luma). Gains up to 2047 work on the
camera: the grain grows in proportion, the frame does not darken, the grain does not invert. At 2047 the deepest
shadows clip to black in about 2 % of the pixels.

## 3. Pixels

side x side bytes, rows from the top, values 0..63 (the range of the camera's own grain tile). The tile is repeated
across the picture, so it must be seamless (periodic). Core takes the integer mean (the field at 42 + 128) / 256 away before
the blend: the grain does not change the brightness.

## 4. What core checks

At power-on (when the look's owner takes the grain service) every header: the magic, version and size, its CRC, the file
size field, the name, the side, the gains, the flags and reserved fields. A container that fails is left out with the
reason in the log. When a tile is chosen its pixels are read once into a single slot: the header again (the file must not
have changed), the pixels' CRC, their largest value, nothing after them. A still taken while the chosen tile is not loaded
yet has no grain (logged).

## 5. The bundled tiles

`resources/modules/filmlook/grain/` holds the tile set that goes into a release: 11 containers and `manifest.json`,
which lists every file with its shown name, side, size, SHA-256, the CRC-32 of its pixels, its gains and the parameters
of the generator recipe that made it (kept for the record; the release uses the files themselves, not the recipes).

| File | Name | Side | Low / Med / High |
|---|---|---|---|
| `CINE250.GRN` | CINE 250D | 512 | 423 / 603 / 844 |
| `CINE500.GRN` | CINE 500T | 768 | 1132 / 1575 / 2047 |
| `CLASS400.GRN` | CLASSIC 400 | 512 | 532 / 886 / 1240 |
| `CLOUD200.GRN` | CLOUD 200 | 768 | 520 / 867 / 1214 |
| `COLOR400.GRN` | COLOR 400 | 512 | 490 / 817 / 1144 |
| `DYE160.GRN` | DYE 160 | 512 | 384 / 548 / 767 |
| `FINE100.GRN` | FINE 100 | 512 | 456 / 760 / 1063 |
| `HALF400.GRN` | HALF FRAME 400 | 512 | 518 / 863 / 1208 |
| `LAB400.GRN` | LAB SCAN 400 | 512 | 653 / 1089 / 1525 |
| `PUSH1600.GRN` | PUSH 1600 | 512 | 617 / 1028 / 1439 |
| `TAB400.GRN` | TABULAR 400 | 512 | 535 / 891 / 1247 |

Every tile carries its own gains (the fields at 48), and the tile's gain is its High. The tiles are made for the physical
scale of a 35 mm frame: one pixel of a 3:2 still is 36 mm / 5184 = 6.94 um of film, and each tile follows the measured
grain spectrum of a film class at that scale. The gains follow the film too: Med is the film's grain deviation in the
mid tones. Film grain is weaker in the shadows and highlights, and the camera follows it with the tone mask (`mask = film`
in `[grain]` of `OMKIT.INI`), which keeps the full gain only in the mid tones. High is 1.4 times Med and Low 0.6 times
Med, but never below a visibility floor; the steps are at least 1.3 apart. The level gains go up to 2047 (the tile's
gain at 46 stays at most 1024); only `CINE500`, the coarsest film, stops at High 2047, under the film (0.60 / 0.83 /
1.08 of it). So the tiles differ both in structure and in strength, as films do.

`tools/grainset.py check` verifies the folder against the manifest (sizes, SHA-256 and the checks of section 4; a `.GRN`
the manifest does not list is an error). `tools/grainset.py copy DEST` checks, then copies the set to `DEST` and removes
any other `.GRN` there; `make -C OMKit card` does this for the card bundle's `OMKIT/GRAIN/`. To use other tiles, put
them in `OMKIT\GRAIN\` on the card next to or instead of these.
