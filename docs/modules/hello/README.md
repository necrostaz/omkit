# HELLO: the example module

HELLO is the module for developers: it does nothing for the user, but exercises the loader and the whole basic API, so
it is the place to start a module of your own ([../../modules.md](../../modules.md#4-your-first-module)). It is not in
the card bundle. Every step writes a log line, so `OMKIT\LOG\CORE.LOG` shows whether each part works.

## 1. What it demonstrates

| Part | What `src/modules/hello/hello.c` does |
|---|---|
| Loader and relocations | tables of pointers to strings and to functions in constants, a pointer to a pointer in data, a `switch` with a jump table, initialised data and `bss`; the results are logged |
| Config | reads `greeting`, `hide` and `fail` from `[HELLO]` (`cfg_str`, `cfg_int`, `cfg_bool`) |
| Store | opens a 64-byte store, counts boots and power-offs in it (persistent records) |
| Memory | allocates and frees a block, logs `mem_info`, then frees a pointer that is not a block's start - `core` refuses and logs it |
| In-memory resource | `res/1_1.txt`, read with `res` |
| Strings | `res/strings.txt` (`NAME = text` lines) becomes `res_strings.h` with `STR_*` ids at build time; reads them with `str`, shows that identical texts are stored once, an empty string and a missing id |
| On-demand resource | `res/disk/2_1.dat` (5000 bytes, byte i = 7i + 3) stays in the module file on the card; reads 8 bytes at offset 4000 and the 3-byte tail with `res_read` |
| Files | writes `HELLO.TXT` into its own folder `OMKIT\HELLO\` (created by `core` at this first file call), then tries an invalid name `..\X` - refused and logged |
| Events | `OMK_EV_PICTMODE_CHECK` in three phases (a pass-through replace, a filter after, a notify after) and `OMK_EV_S1_API`; also two registrations that must fail (a phase the event does not allow, an unknown event). Handlers only count; a deferred call (`defer`) logs a summary, with at most one call pending |
| Power | a power handler that counts power-offs into the store; `core` saves it at the card-off point |
| Failure path | `fail = on` makes `module_init` return an error, to see the module unloaded cleanly |

## 2. Settings

Section `[HELLO]` in `OMKIT.INI`:

| Key | Default | Effect |
|---|---|---|
| `greeting` | `hello` | the text of the first log line |
| `hide` | `-1` (none) | a picture mode number (the camera's interface numbering, [../../platform/om5m2/picture-modes.md](../../platform/om5m2/picture-modes.md#21-raw-value-and-mode-number)) that the filter handler reports as not allowed when the camera builds the picture mode list or steps the control panel cell over it |
| `fail` | `off` | `on` - `module_init` returns 7 and `core` unloads the module |

## 3. Build and run

```sh
make -C OMKit/src/modules MOD=hello VERSION=1.0 OUT=<dir>     # -> <dir>/HELLO.OMM
```

Copy `HELLO.OMM` to `OMKIT\MODULES\`, add `HELLO = on` to `[modules]` and switch the camera on; with
`log_level = debug` in `[core]` the log shows every step. The build and the API: [../../modules.md](../../modules.md),
[../../sdk.md](../../sdk.md).
