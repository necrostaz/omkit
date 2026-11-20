# The .isp profile file, format 1.0

An `.isp` file is a look ("picture profile") prepared for one camera's image signal processor: the colour path tables
in the camera's own formats. The camera applies it by copying tables - no per-pixel work on the CPU - so it works in live
view, in stills and in movies. A tool on a PC makes the file, for example by fitting a film look (a 3D LUT, say)
to the camera's colour path. Reference writer and reader: `apps/ispkit/ispfile.py`.

Status: draft. The meaning of some OM5M2 table fields is still being verified (marked below); a change of their meaning
bumps the platform block layout, not the file format.

## 1. General rules

- All integers are little-endian. Offsets are from the start of the file unless said otherwise.
- The file is a header, a section directory and sections. Every section starts at a multiple of 16 bytes (a cache line):
  it can be read with one aligned read into its own buffer. Padding is zero.
- CRC-32 is the common one (polynomial 0x04C11DB7 reflected, initial and final XOR 0xFFFFFFFF; zlib `crc32`).
- A reader reads the header (64 bytes) first, then the directory, then only the sections it needs: a menu needs the
  header (name) and `ICON`; applying the profile needs `ISPB`.
- Limits: the file is at most 64 KiB, at most 16 sections, the name at most 31 bytes.

## 2. Header (64 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | magic `OISP` |
| 4 | u16 | format major version, 1. A reader rejects another major version |
| 6 | u16 | format minor version, 0. A newer minor only adds sections or uses reserved fields; a reader accepts it |
| 8 | char[8] | target platform, ASCII, zero padded: `OM5M2` (OM System OM-5 Mark II). A reader rejects other platforms |
| 16 | u16 | number of sections |
| 18 | u16 | flags, 0 |
| 20 | u32 | file size in bytes |
| 24 | u32 | offset of the directory (64) |
| 28 | u32 | CRC-32 of the header (with this field read as 0) followed by the directory |
| 32 | char[32] | display name, UTF-8, zero terminated and padded. ASCII is recommended: the camera's text font may lack other characters |

## 3. Directory

One 16-byte entry per section, in any order:

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | tag |
| 4 | u32 | offset of the section |
| 8 | u32 | size of the section's data (without padding) |
| 12 | u32 | CRC-32 of the section's data |

A reader checks the CRC of every section it reads and ignores sections with tags it does not know.

| Tag | Required | Contents |
|---|---|---|
| `ISPB` | yes | the platform's ISP block (section 5) |
| `ICON` | no | pictures for the camera's UI (section 4) |
| `META` | no | provenance: UTF-8 lines `key=value` (for example `fitter`, `variant`, `recipe`, `matrix`, `domain`, `art`); the camera does not read it. The file does not name its source look: the key `look` is not written, and a reader ignores it in older files |
| `CANI` | no | the preset's cassette picture for the FILMLOOK editor (section 5a) |

## 4. ICON: pictures for the UI

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | number of pictures |
| 2 | u16 | 0 |
| 4 | entry[n] | 16 bytes each: u16 kind, u16 width, u16 height, u16 0, u32 offset of the pixels from the section start, u32 size of the pixels |

Pixels are OMKit picture values (`OMK_ICON_*` in `sdk/include/omk_api.h`: 0 clear, 1 sign, 2 outline, 3 and 4 the edge
between them, 5 outside the outline, 6 halo), 4 bits per pixel, the high nibble first, every row a whole number of bytes.
The camera's driver turns them into its own palette, as for module pictures.

| Kind | Picture | OMKit id |
|---|---|---|
| 1 | picture mode icon (the mode strip) | `OMK_PIC_MODE(p, OMK_PROF_ICON_NORMAL)` |
| 2 | large mode icon | `OMK_PIC_MODE(p, OMK_PROF_ICON_LARGE)` |
| 3 | control panel cell | `OMK_PIC_PANEL(p, 0)` |
| 4 | shooting screen sign | `OMK_PIC_SHOOT(p, 0)` |
| 5 | menu line label | `OMK_PIC_LABEL(p, 0)` |

Sizes are the platform's (`prof_picture_size` of the OMKit API); a picture of a wrong size is ignored.

## 5. ISPB for the platform OM5M2

Layout 1, 5064 bytes. The tables replace those of the base picture mode in the colour path:

```
camera RGB after white balance and the camera's matrix -> MATRIX -> GAMMA R/G/B -> YCbCr (BT.601), Cb/Cr gains
-> TONE (on Y) -> 12 SECTORS -> YCbCr
```

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | layout, 1 |
| 2 | u16 | present parts: bit 0 matrix, 1 gamma, 2 tone, 3 Cb/Cr gains, 4 sectors. A part that is not present keeps the base mode's |
| 4 | u16 | base picture mode (`MODE_COLORTONE` value; 2 = Natural): its other settings (sharpness, noise filter and so on) stay |
| 6 | u16 | 0 |
| 8 | s16[9] | the look's matrix F, row-major, Q7 (128 = 1.0); every row sums to 128 (white stays white; a reader refuses another sum). Applied after the camera's white balance and its own colour matrix: OM5M2 core writes S+80 = F x (the camera's matrix of the frame), so the camera keeps its matrix for the light |
| 26 | u16 | 0 |
| 28 | u16 | Cb gain, Q8 (the stock Natural value is 158) |
| 30 | u16 | Cr gain, Q8 (stock Natural 200) |
| 32 | u16[3][256] | gamma curves R, G, B: 256 nodes, values 0..4095. Node i stands for the linear input i/255 of the full scale (*being verified*) |
| 1568 | u16[256] | tone curve on Y: 256 nodes, node i at Y = i/256 (the top 8 bits of Y; above node 255 the last segment continues), values 0..1023 |
| 2080 | u16 | sector mask: bit k - sector k is on (only the records of the sectors that are on are checked) |
| 2082 | u16 | 0 |
| 2084 | u32 | 0 |
| 2088 | record[12] | sectors, 24 bytes each (below) |
| 2376 | s16[12][7][16] | sector weight profiles: node values, Q14 (16384 = 1.0), -16384..16383 - a reader refuses values outside the range; the camera holds them in steps of 1/64 (rounded), up to 63/64. 16 nodes each, per sector in this order: hue for chroma, chroma for chroma, hue for hue, chroma for hue, hue for luma, chroma for luma, luma |

A sector record is the hardware's own (the camera copies it as is):

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | hue window start, 32768 = 360 degrees; 0..32767 |
| 2 | u16 | hue window width, 0..32767 (32767: the whole circle) |
| 4 | u16 | chroma window start, 10-bit chroma, 0..1023 |
| 6 | u16 | chroma window width, 0..1023 |
| 8 | u16 | luma window start, 0..1023 |
| 10 | u16 | luma window width, 0..1023 (0: the window is off, the luma weight is 1). The hardware fields are 10 bit: 1024 would read as 0, so a reader refuses widths over 1023 |
| 12, 14, 16 | u16 | reciprocals of the three widths: `E << 8 | M`, M = round(2^(E+8) / width) in 128..255 |
| 18 | s16 | hue shift, 32768 = 360 degrees |
| 20 | s16 | chroma addition, 10-bit chroma (256 = +0.25 of the full chroma) |
| 22 | s16 | luma addition, 10-bit luma |

The effect of the sectors: Y is the tone curve's output (0..1023; it matches the luma of the JPEG), Cb and Cr follow
the gains (+-512), C = |Cb, Cr|, h = atan2(Cr, Cb) in 1/32768 turns. For every sector and operation the weight is the
product of its hue and chroma profiles at the pixel's hue and chroma and of the sector's luma profile at the pixel's luma
(all three operations, shown on the camera). A profile's 16
nodes split its window into 16 equal parts; the weight is linear between the nodes, and from node 15 it falls to 0 at
the window's edge - for a hue window over the whole circle it runs to node 0 instead; the weight is 0 outside the window.
The camera builds the hardware's profile words from the node values (the node value and the slope to the next node).
Sectors 0..5 and 6..11 are two banks: within a bank a pixel gets one sector - the lowest-numbered whose hue and chroma
windows hold it - and the two banks add up: chroma += addition x weight, hue += shift x weight, luma += addition x weight
for that sector of each bank.

### 5.1. Black-and-white blocks

Whether a block makes a black-and-white picture is decided by one rule, `omk_ispb_mono()` in the SDK header
`omk_ispb.h` (core and the modules use the same function; the header also holds the block's offsets and present bits). A
block is black and white when either holds:

- it sets the Cb/Cr gains (present bit 3) and one of them is 0 - no colour difference is left;
- it sets the matrix (present bit 0) and its rows differ by at most 2 (Q7 steps) in every column - R, G and B come out as
  one mix of the camera's channels.

A part the block does not set is not looked at: its field may hold 0. A block with neither part keeps the matrix and the
gains of its base picture mode and counts as colour. A tint made by the gamma curves or the sectors (a toned black and
white) does not make a block colour.

Measured on 22 reference presets: the largest difference between matrix rows is 0 for Tri-X (its gains are 0 as well), 1
for Double-X (toned by its gamma curves) and 91..130 for the 20 colour ones. The camera's own black-and-white filter
matrices do not have equal rows (the camera removes the colour by other means), so a filter written into a block whose
gains are not 0 makes it colour by this rule - and in the picture as well.

Uses: core gives no colour grain to the stills of a black-and-white look (core 1.21); the look's editors choose between the
black-and-white filter and the colour saturation by it.

## 5a. CANI: the cassette picture

A drawing the FILMLOOK module shows in the picture mode screen's editor for this preset (core >= 1.01, API `look_art`),
in place of the rectangle with the preset's name. It is a list of OpenVG paths the camera replays, no images. Generic
cassettes carry the profile's name and ISO drawn in, so the camera prints no text into them (the generator:
`apps/ispkit/canister.py`). All numbers little endian.

Header, 64 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | version, 1 |
| 2 | u16 | kind, 1 - a drawing |
| 4 | u16, u16 | the surface the coordinates are for: 256, 256 |
| 8 | u16 | records, 1..64 |
| 10 | u16 | flags, 0 |
| 12 | u32 | bytes of the records after the header, at most 6144; the section is exactly 64 + this |
| 16 | char[16], char[24], char[8] | brand, name, ISO: plain ASCII, zero padded (logs and tools; not drawn) |

A record, 16 bytes and its data, in drawing order:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | operation: 1 fill (non-zero), 2 fill (even-odd), 3 stroke |
| 1 | u8 | stroke caps: 0 butt, 1 round, 2 square |
| 2 | u16 | 0 |
| 4 | u32 | colour RGBA8888, R in the high byte (as `vgSetColor`) |
| 8 | u32 | stroke width: the bits of a float, px (0x3c000000..0x41800000); 0 for a fill |
| 12 | u16 | segments, 1..352 |
| 14 | u16 | points, 0..320: one per MOVE_TO and LINE_TO, two per QUAD_TO, three per CUBIC_TO |
| 16 | u8[n] | OpenVG absolute segments: MOVE_TO 2 (the first one), LINE_TO 4, QUAD_TO 10, CUBIC_TO 12, CLOSE 0; zero padded to 4 |
| ... | s16[2m] | points: x, y in quarter pixels, 0..1024, y up |

The camera checks everything above before it keeps the picture (core `cani_check`); a section that fails is refused
with the reason in the log and the preset shows the rectangle. The picture is drawn centred across the editor's surface
and lifted 66 pixels towards its top, so that a cassette drawn at rows 84..179 (as the generator draws them) starts at
row 18, where the curve editor's window starts (core 1.29; 80 pixels before), with the preset's number "n / N" under it.
The data is not changed for this: the lift is the camera's.

## 6. Reading on the camera

1. Read 64 bytes. Check the magic, the major version, the platform, the size against the file size.
2. Read the directory, check the header CRC.
3. For a menu: the name from the header, the `ICON` section if present.
4. To apply: read `ISPB` into an aligned buffer, check its CRC, its layout and size; then hand the tables to the colour
   path (the platform driver).
5. On any mismatch: refuse the file, log why, keep the camera's own tables.

## 7. The bundled preset set

`resources/modules/filmlook/presets/` holds the FILMLOOK presets that go into a release: 32 files and `manifest.json`
(each file's name, profile name, size and SHA-256, the SHA-256 of its `ISPB` and the cassette's header texts). FILMLOOK
lists them in the order of their file names, without case, so the variants of one film stand together
(`E100DAY`, `E100NIGHT`).

Naming rules of the set:

- Profile name (header): `Brand Film ISO [variant]`, the brand's and film's own spelling (`Kodak Tri-X 400 HC`,
  `Fujifilm X-Tra 400 Expired`), no number repeated, no spaces at the ends. A look that is not one maker's film has no
  brand (`Old Ortho 80 Toned`).
- Cassette (`CANI`): one of the generic pictures with the maker in capitals on the brand line (`KODAK`, `FUJIFILM`,
  `ILFORD`; `B&W 135` when there is no maker), the film and its variant on the name line (`KODACHROME EXP`, `X-TRA SCAN`; at most 14 characters, the full font),
  the speed in the ISO box and the process on the last line (`C-41`, `E-6`, `ECN-2`, `K-14`, `B&W`).
- `META` is plain ASCII and has no `look` key.

`tools/presetset.py check` verifies the folder against the manifest (sizes, SHA-256, the checks of section 6, the
profile name and the rules of `META`; a `.ISP` the manifest does not list is an error). `tools/presetset.py copy DEST`
checks, then copies the set to `DEST`; other `.ISP` files there are kept, they are the user's own. `make -C OMKit card`
does this for the card bundle's `OMKIT/FILMLOOK/`.
