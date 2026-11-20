# Building OMKit

How to build `core`, the modules and the card bundle from source, run the tests and check the bundled data. Camera
owners do not need any of this: a release ZIP holds everything built ([platform/om5m2/fw1100/install.md](platform/om5m2/fw1100/install.md)).

Clone the repository into a folder named **`OMKit`** (`git clone https://github.com/necrostaz/omkit OMKit`): a few commands use the
folder name as a Python package name (`python -m OMKit....`). The commands are run from the folder that holds
`OMKit/` (`make -C OMKit ...`); inside `OMKit/` drop the `-C OMKit`.

## 1. Requirements

### 1.1. Host

| Host | State |
|---|---|
| Linux, Debian or Ubuntu | supported; tested on Ubuntu 24.04 |
| Windows | through WSL 2 with a Debian or Ubuntu distribution; tested with Ubuntu 24.04. Run everything inside WSL, on a checkout under `/mnt/c/...` or in the WSL file system (faster) |
| macOS | not tested; hints in [1.4](#14-macos-not-tested) |
| other Linux distributions | not tested; the same tools under their own package names |

### 1.2. Packages (Debian, Ubuntu, WSL)

```sh
sudo apt install build-essential bzip2 xz-utils wget python3 python3-venv
```

| Package | For |
|---|---|
| `build-essential` | the host C and C++ compilers, `make` and the C library headers: the toolchain build and the host tests |
| `bzip2`, `xz-utils`, `wget` (or `curl`) | downloading and unpacking the toolchain sources |
| `python3` | the build tools (`tools/`), the data checks, the tests |
| `python3-venv` | a virtual environment for numpy and Pillow (the apps, the picture generator, the full `make test`): Ubuntu 24.04 has no `pip` for the system Python and refuses `pip install` into it |

Nothing else is needed for the toolchain: GCC's own libraries (gmp, mpfr, mpc, isl) are built inside the GCC tree, and
`texinfo`, `bison`, `flex` and `m4` are not used (the toolchain build was verified on a host without them).

On Debian and Ubuntu the commands of this guide and of the apps' pages are written with `python`; there it is called
`python3` (or install `python-is-python3`), and inside a virtual environment both names work.

### 1.3. Python

| What | Python | Packages |
|---|---|---|
| build tools: `tools/pack.py`, `mkmod.py`, `grainset.py`, `presetset.py`, `cardini.py`, `mkicons_film.py`, `dial_label.py`; `tests/test_mkmod.py` | 3.9 or newer | standard library only |
| `tools/mkicons_creatprf.py` (CREATPRF's pictures; run by hand, and by `make test` as a check) | 3.9 or newer | numpy |
| `release/build_release.py` (release ZIP) | 3.10 or newer | standard library only |
| `release/user/omkit_firmware.py` (the camera owner's firmware tool) | 3.9 or newer | standard library only |
| `apps/` (ISP player, fitter, neurofit) | 3.10 or newer | numpy, Pillow, rawpy: `apps/requirements.txt` ([apps/README.md](apps/README.md#install-once)) |

The minimum versions come from the language and library features the code uses (checked with a static analysis);
the tests run with Python 3.11 to 3.13. What needs numpy or Pillow is optional: without them `make test` skips those
checks and says so.

To get numpy and Pillow without touching the system Python:

```sh
python3 -m venv ~/venv/omkit && ~/venv/omkit/bin/python -m pip install -r OMKit/apps/requirements.txt
make -C OMKit test PYTHON=~/venv/omkit/bin/python
```

### 1.4. macOS (not tested)

Hints only: the Xcode command line tools (`xcode-select --install`) and Homebrew `make` (GNU make 4, run as `gmake`),
`wget`, `xz`, `coreutils` (`sha256sum`, used by `make card`). The toolchain script falls back to `shasum` and `sysctl`.
GCC 13.3 may not build as a host program on Apple Silicon; a Linux virtual machine or container is the dependable way.

## 2. The toolchain

`core` and the modules are compiled for the camera's CPU (Panasonic MN10300 family) with GNU binutils 2.45 and GCC 13.3.0
for `mn10300-elf`. Build it once:

```sh
bash OMKit/toolchain/build-toolchain.sh                                  # into ~/opt/mn10300
PREFIX=/opt/mn10300 WORK=/tmp/mn10300 bash OMKit/toolchain/build-toolchain.sh   # elsewhere
```

| Variable | Default | Meaning |
|---|---|---|
| `PREFIX` | `~/opt/mn10300` | where the toolchain is installed. The Makefiles look there; another prefix needs `TC=<PREFIX>/bin/mn10300-elf-` on every `make` |
| `WORK` | `~/build/mn10300-toolchain` | downloads, sources, build folders, logs (`WORK/logs/`); can be removed afterwards |
| `JOBS` | the number of CPUs | parallel `make` jobs |
| `GNU_MIRROR` | `https://ftp.gnu.org/gnu` | where the binutils and GCC archives come from |
| `FORCE` | `0` | `1` builds again over an installed toolchain |

What it does:

- checks the host tools first and names the missing ones with the packages to install;
- downloads `binutils-2.45.tar.xz` and `gcc-13.3.0.tar.xz` and checks them against SHA-256 sums pinned in the script
  (the same archives carry valid GNU signatures). An archive already in `WORK` is checked and reused;
- fetches GCC's libraries with GCC's `contrib/download_prerequisites` from `gcc.gnu.org` (not from `GNU_MIRROR`),
  which checks them against the SHA-512 sums inside the GCC archive and skips an archive already in the GCC source
  folder. A build without network therefore needs both: the binutils and GCC archives in `WORK`, and the gmp, mpfr,
  mpc and isl archives that `contrib/prerequisites.sha512` of GCC names in `WORK/src/gcc-13.3.0/`;
- configures, builds and installs binutils, then GCC for C only with `libgcc` (no C library: OMKit needs none);
- is safe to run again: an installed toolchain is detected and left alone, a finished step is skipped, an interrupted
  one is redone. A failed step prints the end of its log and the log's path.

| Resource | Measured |
|---|---|
| time | about 3 minutes with 24 parallel jobs, including the downloads (Ubuntu 24.04 in WSL); proportionally longer on fewer cores |
| download | about 125 MB |
| disk | `WORK` about 3.5 GB, `PREFIX` 0.8 GB |

The versions are pinned: OMKit's binaries are built and tested with exactly these, and the same sources give the same
bytes. The MN10300 target has no LRA register allocator, so newer GCC branches may drop it.

## 3. Building

### 3.1. Commands

```sh
make -C OMKit                                           # core          -> OMKit/build/CORE.BIN
make -C OMKit/src/modules MOD=filmlook VERSION=1.12     # one module    -> OMKit/build/modules/FILMLOOK.OMM
make -C OMKit card                                      # card bundle   -> OMKit/build/card/OMKIT/
make -C OMKit test                                      # tests and checks (section 5)
make -C OMKit clean                                     # removes OMKit/build (OUT)
```

A module kept outside this tree builds with the same Makefile; `MOD` is its name, `MODDIR` its source folder (every
`*.c` there, `res/` and `res/disk/` for its resources):

```sh
make -C OMKit/src/modules MOD=mymod MODDIR=/path/to/mymod VERSION=1.0 OUT=/path/to/out   # -> /path/to/out/MYMOD.OMM
```

The card bundle holds `CORE.BIN`, the bundled modules (`RELEASE_MODULES` in `OMKit/Makefile`, with their versions), the
grain tiles in `GRAIN/` and the FILMLOOK presets in `FILMLOOK/`; both sets are checked against their manifests before
they are copied, and the recipe ends with the SHA-256 of every file. It is always built without `RES_OVER`. It also
holds `OMKIT.INI`: the sample `core` writes (`cfg_template` in `src/core/cfg.c`) with the bundled modules on, made by
`tools/cardini.py` ([config.md](config.md#1-where-the-file-comes-from)), so the modules run from the first start. Copy
`OMKIT/` to the card's root.

### 3.2. Outputs

| Output | Default place |
|---|---|
| `core` (`CORE.BIN`; `core.elf`, `core.map`, `core.body`, the object files) | `OMKit/build/` |
| a module (`<NAME>.OMM`; `<mod>.elf`, `<mod>.map`, `<mod>.gen/res_strings.h`) | `OMKit/build/modules/` |
| the card bundle | `OMKit/build/card/OMKIT/` |
| the test programs and the test build of `HELLO` | `OMKit/build/test/` |
| a release ZIP ([release.md](release.md)) | `OMKit/build/release/` |

`OMKit/build/` is ignored by git.

### 3.3. Variables

| Variable | Makefile | Default | Meaning |
|---|---|---|---|
| `OUT` | both | `build` (core), `../../build/modules` (modules) | the output folder. Relative paths are relative to the Makefile's folder (`OMKit/` or `OMKit/src/modules/`): pass an absolute path to be safe |
| `EXTRA` | both | - | more compiler options, e.g. `EXTRA=-DDEBUG` |
| `MOD` | modules | `hello` | the module's folder name; upper-cased it is the module and file name |
| `MODDIR` | modules | `<MOD>` | the module's source folder, for a module outside `src/modules/` |
| `VERSION` | modules | `1.0` | `major.minor`, stored in the `.OMM` |
| `RES_OVER` | modules | - | a folder laid out as `res/` whose resources replace the module's own of the same type and id; for private experiments only, never in the card bundle |
| `PLATFORM` | core | `om5m2/fw1100` | the driver `src/platform/<platform>/<firmware>/` (the only one so far) |
| `TC` | both | `~/opt/mn10300/bin/mn10300-elf-` | the toolchain's tool prefix |
| `PYTHON` | both | `python3` | the Python that runs the tools and the Python tests |
| `HOST_CC` | core | `gcc` | the host compiler of the host tests |

A change of `VERSION` or of the resource options rebuilds the module; a change of `EXTRA` does not, so run `make clean`
(or use another `OUT`) when switching options. Module sources build with `-Wall -Wextra -Werror`; `core` with
`-Wall -Wextra`.

### 3.4. The loader patch description (maintainers only)

Not part of a normal build: `make`, `make card`, `make test` and the release build neither need nor change it.
`python -X utf8 -m OMKit.src.stub.platform.om5m2.fw1100.gen` (run from the folder that holds `OMKit/`) writes
`src/stub/platform/om5m2/fw1100/spec.json`, the description of the loader patch
([platform/om5m2/fw1100/patch.md](platform/om5m2/fw1100/patch.md)). `spec.json` and the loader source `stub.s` are
the input of the maintainers' patch builder, which needs the official firmware and is not part of OMKit; it makes the
release's `patch.json` ([release.md](release.md#2-patchjson)). The patched firmware itself is made by the release tool
on the user's computer.

## 4. Pictures and data sets

### 4.1. Module pictures

The pictures are committed in `resources/modules/<module>/icons/`, and a module build takes them from there. The
generators are deterministic; run them only to change the pictures, then commit the result:

```sh
python3 OMKit/tools/mkicons_creatprf.py OMKit/resources/modules/creatprf/icons           # CREATPRF (needs numpy)
python3 OMKit/tools/mkicons_film.py OMKit/resources/modules/filmlook/icons               # FILMLOOK
python3 OMKit/tools/mkicons_creatprf.py OMKit/resources/modules/creatprf/icons --check   # compare, write nothing
python3 OMKit/tools/mkicons_film.py OMKit/resources/modules/filmlook/icons --check
python3 OMKit/tools/dial_label.py Level Prev                                             # a dial label, as text
```

Every module's dial label is drawn by `tools/dial_label.py` in the face of the camera's own labels
([platform/om5m2/ui.md](platform/om5m2/ui.md), section 4.5).

### 4.2. Grain tiles and presets

The bundled grain tiles (`resources/modules/filmlook/grain/`) and FILMLOOK presets (`resources/modules/filmlook/presets/`)
each have a `manifest.json` with every file's size and SHA-256. The check tools verify the files against it and parse
every container the way the camera does; a file the manifest does not list is an error too:

```sh
python3 OMKit/tools/grainset.py check                   # 11 tiles: OK
python3 OMKit/tools/presetset.py check                  # 32 presets: OK
python3 OMKit/tools/grainset.py copy <card>/OMKIT/GRAIN # check, then copy (make card does this)
```

Formats: [formats/grn.md](formats/grn.md), [formats/isp.md](formats/isp.md); the sets:
[modules/filmlook/grain.md](modules/filmlook/grain.md), [modules/filmlook/presets.md](modules/filmlook/presets.md).

## 5. Tests

```sh
make -C OMKit test
```

| Part | What runs | Needs |
|---|---|---|
| host tests (`tests/host/run.sh`) | parts of `core` (config, log, clock, store, memory, power) built with the host compiler against the stub driver `tests/host/host_drv.c`; the module API header declares no driver; FILMLOOK's black-and-white rule of an ISP block, its `.isp` file checks and CRC-32 on the bundled presets (`test_ispfile.c`), and its start against a stand-in for `core` - the order of file reads, the look brought back early, the stored edits - and its tables under the editors, byte for byte, with the pacing of quick edits (`test_filmlook.c`); the driver's plain C parts: the grain texture's fill by rows against the word-by-word one (`test_texfill.c`) and the editors' quarter sine table against the full one (`test_edtrig.c`) | host `gcc` |
| | the module loader with a real `HELLO.OMM` (relocation, API, refusals, unloading); a module that refers to the firmware does not link, and `mkmod.py` refuses it | the MN10300 toolchain (skipped with a message without it) |
| `tests/test_mkmod.py` | the `.OMM` file of `HELLO`, the string table, `--res-over` | the toolchain for the `HELLO` parts |
| data checks | `grainset.py check`, `presetset.py check`, `mkicons_film.py --check`; `mkicons_creatprf.py --check` | numpy for the last one (skipped without it) |
| apps | `python3 -m unittest discover -s apps -t apps` | numpy and Pillow (skipped without them) |

Each part prints its result; the target fails on the first failing part and ends with `make test: all passed`. The
parts can be run alone: `make -C OMKit test-host`, `test-tools`, `test-data`, `test-apps`.

## 6. Release and apps

- A release ZIP for camera owners: `python3 OMKit/release/build_release.py --version <x.y.z> [--test]` builds the card
  bundle itself and writes to `OMKit/build/release/` ([release.md](release.md)).
- The apps for the computer need no build, only their Python packages: [apps/README.md](apps/README.md).

## 7. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `build-toolchain: missing host tools: ...` | install the packages it names (section 1.2) |
| `step 'gcc-build' failed; the full log: ...` | read the log; the usual causes are a full disk (`WORK` needs about 3.5 GB: set `WORK` to a larger disk) or missing host packages. Run the script again after the fix: it continues |
| `SHA-256 ..., expected ...` | a broken or different archive: delete it from `WORK` and run again, or set `GNU_MIRROR` to another GNU mirror |
| `OMKit: no MN10300 compiler .../mn10300-elf-gcc` | build the toolchain (section 2), or pass `TC=<prefix>/bin/mn10300-elf-` when it is installed elsewhere. The Makefiles do not search `PATH` |
| `skip  module loader and firmware symbol checks: no MN10300 toolchain` | the same: `make test` found no `$(TC)gcc` |
| a module build stops on a warning (`cc1: all warnings being treated as errors`) | module sources build with `-Werror`: fix the warning (e.g. `(void)param;` for an unused parameter). For a quick experiment only: `EXTRA=-Wno-error` |
| `set: pipefail: invalid option name`, `set: Illegal option -`, `/usr/bin/env: 'bash\r': No such file or directory`, `$'\r': command not found`, or a data check failing on files you did not change | Windows line ends (CR LF) in a checkout used from WSL. `.gitattributes` makes git check every text file out with LF and leaves the data files binary, but a checkout made without it, an archive unpacked on Windows or an editor that saves CR LF bring them back. Make a fresh checkout, or in the checkout (after committing or stashing your changes): `git rm -r -q --cached . && git reset -q --hard` |
| `skip  mkicons_creatprf.py --check: numpy is not installed`, `skip  the apps' tests` | install numpy and Pillow for the Python that `make` runs, or point `PYTHON` at one that has them (section 1.3) |
| `error: externally-managed-environment` from `pip` | Debian and Ubuntu protect the system Python: use a virtual environment (section 1.3) |
| `python3: command not found` | install `python3`, or pass `PYTHON=<your python>` |
| `ld: warning: ... has a LOAD segment with RWX permissions` | expected for `core.elf` and for a module: each image is one segment, code and data together; not an error |
| `make: warning: Clock skew detected` | seen on a Windows drive under WSL (`/mnt/c/...`), whose file times lag behind; harmless. A checkout in the WSL file system avoids it |
| `make card` fails in `grainset.py` or `presetset.py` | a tile or preset differs from its manifest: restore the folder from git (`git checkout -- OMKit/resources`) |
| `make card` fails in `cardini.py` | `cfg_template` in `src/core/cfg.c` is no longer a plain list of string literals with one `[modules]` section holding only `enabled`, or a `RELEASE_MODULES` name is not a module name: the message says which; adapt the tool with the sample |

## 8. The source tree

| Directory | Contents |
|---|---|
| `src/core/` | Core in C, independent of the camera (`include/`: its headers and the driver interface `omk_drv.h`) |
| `src/platform/<platform>/<firmware>/` | Driver: addresses, thunks, firmware check (`om5m2/fw1100`: OM-5 Mark II, firmware 1.1) |
| `src/sdk/` | Everything a module sees: `include/omk_api.h`, `omk_types.h`, `omk_events.h`, `lib/omk_libc.c` |
| `src/modules/` | Modules (`creatprf`, `filmlook`, `hello`): `<name>/*.c`, `<name>/res/` (optional); `Makefile`, `module.ld` |
| `src/stub/platform/<platform>/<firmware>/` | Loader: a patch to the stock firmware - `gen.py` writes `spec.json`, the hook body `stub.s` |
| `resources/modules/<name>/` | Files the modules need, kept in git: `creatprf/icons/` and `filmlook/icons/` (the modules' pictures), `filmlook/grain/` (the bundled grain tiles), `filmlook/presets/` (the bundled FILMLOOK presets) |
| `apps/` | Programs for the computer ([apps/README.md](apps/README.md)): `isp_player/`, `isp_fitter/`, `isp_neurofit/` and their shared library `ispkit/`; `requirements.txt` |
| `tools/` | Packing of `CORE.BIN` (`pack.py`), module builder `mkmod.py`, picture generators `mkicons_creatprf.py` and `mkicons_film.py` (with `dial_label.py`, the dial labels' face), grain tile set and preset set check and copy `grainset.py`, `presetset.py`, the card's `OMKIT.INI` `cardini.py` |
| `release/` | Release build ([release.md](release.md)): `build_release.py` makes the ZIP for camera owners; `user/` the firmware tool `omkit_firmware.py` and its launchers for Windows, macOS and Linux; `platforms/<platform>/<firmware>/` the platform data and the patch as data (`platform.json`, `patch.json`) |
| `tests/` | Tests run by `make test`: `host/` tests of core and module parts on the host, with a stub driver (`run.sh` runs them); `test_mkmod.py` tests of the module builder |
| `toolchain/` | `build-toolchain.sh`: the MN10300 toolchain (binutils and GCC), section 2 |
| `docs/` | Documentation ([README.md](README.md)); `formats/`: the file formats [`.isp`](formats/isp.md) and [`.GRN`](formats/grn.md) |

Another platform or firmware gets its own `src/platform/<platform>/<firmware>/` (driver) and
`src/stub/platform/<platform>/<firmware>/` (loader).
