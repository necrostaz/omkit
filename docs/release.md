# Building an OMKit release

For maintainers. A release is one ZIP for one platform and firmware (now `om5m2/fw1100`: OM-5 Mark II, firmware 1.1)
that a camera owner extracts and follows. It holds no firmware: the owner's computer makes the update files from the
official file with the release's firmware tool. User side: [platform/om5m2/fw1100/install.md](platform/om5m2/fw1100/install.md).

## 1. Layout

| Path | What |
|---|---|
| `release/build_release.py` | Builds the ZIP (section 3) |
| `CHANGELOG.md` | The changes of every release ([Keep a Changelog](https://keepachangelog.com/en/1.1.0/)); goes into the ZIP, and the release's section is the text of its GitHub release (section 3.1) |
| `release/user/omkit_firmware.py` | The firmware tool for users: platform-independent, Python 3.9+, standard library only |
| `release/user/get_firmware_windows.bat`, `get_firmware_macos.command`, `get_firmware_linux.sh` | Launchers: find Python 3.9+ (or say how to install it), run the tool, wait for a key at the end |
| `release/platforms/<platform>/<firmware>/platform.json` | Platform data: camera name, model code, the server's model fields, firmware version, the official file's name, size and SHA-256, the card folder and update file names, the install guide's path |
| `release/platforms/<platform>/<firmware>/patch.json` | The patch as data (section 2) |
| `release/platforms/<platform>/<firmware>/INSTALL.txt` | The ZIP's short step-by-step install guide (plain text, CRLF, at most 78 characters a line; `{version}` and `{test}` filled in by the builder). The full guide stays in `docs/platform/<platform>/<firmware>/install.md` |

## 2. `patch.json`

The edits of the loader patch, as the firmware tool applies them:

| Field | Meaning |
|---|---|
| `input.size`, `input.sha256` | The official firmware file. The tool accepts no other file, so the state before the edits is fixed by this hash alone |
| `components[]` | For each changed component: `kind` (`0x0100` = main code), `payload_size`, `payload_sha256_before` and `_after` (SHA-256 of the decoded payload), `checksum_after` (for information), `edits[]` |
| `edits[]` | `name`, `address` (execution address, for information), `offset` (in the decoded payload, hexadecimal), `size`, `bytes` (the new bytes as hexadecimal byte pairs, 16 a line). Only new bytes: the loader's code and data. No bytes of the official firmware are ever stored |
| `output.components` | The components the update file keeps, in file order (now main only: the camera accepts an update file with a subset of the components and writes only those) |
| `output.size`, `output.sha256` | The resulting update file |

The tool decodes the container (each component's payload is stored in 16-byte groups, permuted and inverted; the
32-bit sum of header, decoded payload and trailer is 0), checks every component's sum, writes the edits, checks the
payload hashes, encodes the changed component with a new sum, parses the result again and compares its SHA-256 with
`output.sha256`. It writes nothing unless every check passes.

`patch.json` is generated, not written by hand: the maintainers build the patched firmware with their patch builder
from `src/stub/platform/<platform>/<firmware>/spec.json` and `stub.s` (it needs the official firmware and private
tables of free and forbidden firmware areas, so it is not part of OMKit), check that the result is byte for byte the
build that was tested on a camera, and emit the edits. A changed loader therefore needs a camera test before a new
`patch.json` can be released.

## 3. Building

Prerequisites: Python 3.10+ for the builder; the MN10300 toolchain for `make card` (Linux or WSL:
`bash OMKit/toolchain/build-toolchain.sh` once; [build.md](build.md)); `OMKit/LICENSE`, `OMKit/LICENSE-CC-BY-4.0` and `OMKit/NOTICE` for a real release; a clean git tree.

```sh
python3 OMKit/release/build_release.py --version 0.1.0              # a release
python3 OMKit/release/build_release.py --version 0.1.0 --test       # a TEST build (no LICENSE needed, tree may be modified)
python3 OMKit/release/build_release.py --version 0.1.0 --card DIR   # reuse a built OMKIT folder
```

Without `--card` the builder runs `make -C OMKit card BUILD=release` with its own `OUT` and `CARD` in the output folder
(on Windows through `wsl bash -lc`): a release is always a release build ([build.md](build.md#35-release-and-debug-builds)),
and `make card` checks the images' kind with `tools/buildcheck.py`. Output: `OMKit/build/release/` (next to the Makefile's default `OMKit/build/`; `--out DIR` for another folder):
`OMKit-<version>-<platform>-<firmware>.zip`, its `.sha256` and `.notes.md`, the text of the GitHub release
(section 3.1). A `--test` build is named `...-TEST.zip`, carries
`TEST-BUILD-NOT-FOR-PUBLISHING.txt` and a banner in `INSTALL.txt`, and `RELEASE.json` says `"test": true`: never publish
it.

The ZIP has one top folder of the same name:

```
OMKit-<version>-om5m2-fw1100/
  INSTALL.txt  CHANGELOG.md  RELEASE.md  RELEASE.json  LICENSE  LICENSE-CC-BY-4.0  NOTICE  SHA256SUMS.txt
  OMKIT/CORE.BIN  OMKIT/OMKIT.INI  OMKIT/MODULES/*.OMM  OMKIT/GRAIN/*.GRN  OMKIT/FILMLOOK/*.ISP
  firmware_tool/omkit_firmware.py  platform.json  patch.json  get_firmware_windows.bat  get_firmware_macos.command
                get_firmware_linux.sh
```

`OMKIT/` is the card bundle exactly as `make card` built it. Its `OMKIT.INI` turns the bundled modules on, so a camera
owner has them running from the first start without editing anything. `make card` writes it with
`tools/cardini.py write <card>/OMKIT.INI $(RELEASE_MODULES)`: the tool reads the sample that `core` writes when a card
has no `OMKIT.INI` (the C string `cfg_template` in `src/core/cfg.c`), keeps every line of it (its comments explain the
keys), keeps `enabled = on` in `[modules]` and adds after the sample's `[modules]` lines a comment and one `NAME = on`
line per module of `RELEASE_MODULES`, in that order (the load order). It then checks the text by `core`'s own parser
rules: the sample's CR LF line ends, at most `CFG_MAX` (8192) bytes and `CFG_ENTRIES` (128) entries, no line `core`
would not understand, every module on. The file is generated rather than kept in the tree so that it can never drift
from `core`'s sample; it is made by `make card` rather than by the builder so that a card built for testing is the
release's card. `core` itself is unchanged: it still writes its own sample, which loads no module, only when the card
has no `OMKIT.INI`. A user who updates keeps their own `OMKIT.INI` (the install guide, "Updating OMKit"): when a release
adds a module, its section in `CHANGELOG.md` must say so and give the line to add (the install guides send updating
users there).

### 3.1. The changelog and the GitHub release

`CHANGELOG.md` collects the changes as they are made, under `## [Unreleased]`, in the groups `### Added`, `Changed`,
`Deprecated`, `Removed`, `Fixed` and `Security`, written for camera owners. It holds only absolute links: the same text is
read in the ZIP and on GitHub, where a link into `docs/` would lead nowhere. To release a version:

1. Check that the OMKit repository's address (https://github.com/necrostaz/omkit) is current in the texts a user of the ZIP reads:
   `INSTALL.txt` of the platform (its last lines name the full guide), the README and `docs/build.md` (the clone
   command).
2. Rename the Unreleased section to `## [<version>] - <YYYY-MM-DD>` and put an empty `## [Unreleased]` above it.
3. Commit, then build the release (section 3).
4. Publish a GitHub release with the tag `v<version>`, the title `OMKit <version>`, the ZIP and its `.sha256` as
   files, and the text of `OMKit-<version>-<platform>-<firmware>.notes.md`: that version's section of the changelog -
   only what changed since the previous release - followed by the component versions, the install pointer (for an
   update: no flashing, copy the `OMKIT` files but `OMKIT.INI`) and the ZIP's SHA-256.

The builder refuses a release whose version has no dated section, is not the newest one, or that leaves changes under
Unreleased; it also checks the form of the whole file (the title, the headings, the groups, unique versions, real
dates, no relative links). A `--test` build takes the version's section if there is one, else Unreleased, else the newest release's section.

The ZIP is reproducible: entries sorted, times from the last commit of `OMKit/`, Unix permissions (the `.sh` and
`.command` launchers executable), `.bat` and `INSTALL.txt` with CR LF (so that any text editor shows the guide right) and the other texts with LF line
ends.

## 4. What the builder checks

| Check | Refused when |
|---|---|
| Version | not `MAJOR.MINOR.PATCH[-suffix]` |
| Tree | uncommitted changes under `OMKit/` (not for `--test`) |
| Licence | no `OMKit/LICENSE`, `LICENSE-CC-BY-4.0` or `NOTICE` (not for `--test`) |
| `CHANGELOG.md` | not in the form of section 3.1; for a release (not `--test`): no dated section for the version, not the newest, or changes left under Unreleased |
| `CORE.BIN` | bad header, length or sum; not the `CORE_VERSION` of `src/core/include/omk_core.h` |
| Modules | not exactly the `RELEASE_MODULES` of the Makefile, or a module's name or version differs |
| Grain tiles, presets | `tools/grainset.py check` or `tools/presetset.py check` fails, or the card's files differ from the manifests |
| `OMKIT.INI` | not byte for byte what `tools/cardini.py` makes from `core`'s sample and `RELEASE_MODULES` |
| Card folder | anything besides `CORE.BIN`, `OMKIT.INI`, `MODULES`, `GRAIN`, `FILMLOOK` |
| `platform.json`, `patch.json` | they disagree on the official file; an edit with any field besides the new bytes; more than 4 KiB of edits |
| No firmware | any file that starts like a firmware component, is larger than 16 MiB, or is named like an official or update file |

`SHA256SUMS.txt` lists every other file of the ZIP (`sha256sum -c SHA256SUMS.txt` in the extracted folder).

## 5. A new platform or firmware

1. The driver `src/platform/<platform>/<firmware>/` and the loader `src/stub/platform/<platform>/<firmware>/` (see
   [architecture.md](architecture.md#10-adding-a-platform-or-a-firmware-version)), tested on the camera.
2. `release/platforms/<platform>/<firmware>/platform.json`: model code, the server's model fields (`hardware_kind`,
   `model_id`), the container's model bytes, the firmware version, the official file's name, size and SHA-256, the
   update folder and file name of the camera.
3. `patch.json`, generated from the camera-tested loader build (section 2).
4. `docs/platform/<platform>/<firmware>/install.md`, the full install guide, with every fact from the platform pages,
   and `release/platforms/<platform>/<firmware>/INSTALL.txt`, the short step-by-step guide of the ZIP.
5. A card target that builds that platform's `CORE.BIN` (the Makefile's `PLATFORM`).
6. `build_release.py --platform <platform>/<firmware> --test`, then the end-to-end check: the firmware tool from the
   extracted ZIP must produce the tested update file.
