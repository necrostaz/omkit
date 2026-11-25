# DEBUG: the RAM log and the USB debug channel

`core` 1.40 has a debug service for developers. It is **off** unless `OMKIT.INI` turns it on, and the release's
`OMKIT.INI` does not. With it on:

- the log of `core` and of the modules, and short snapshots of the camera's file and recording tasks, also go to a
  ring buffer in RAM that survives a reset of the camera (not a power-off and not the battery taken out). After a
  crash and restart, the RAM still holds the log of the run that went down, up to its last moment;
- a computer connected by USB in the **MTP** mode reads that RAM log, the live state of the camera's tasks and memory,
  and the card's files, with `tools/omkdebug.py` (Windows);
- optionally, `core` writes nothing to the card at all (the log and the stores stay in RAM), so that the card is out of
  the picture while a problem is looked for;
- optionally, the computer writes files of the `OMKIT` folder (a new `CORE.BIN`, modules, presets, `OMKIT.INI`), each
  one checked by a CRC-32 read back from the card before it replaces the old file, restarts the camera so that they
  run, and starts or stops the test scenario ([config.md](config.md#10-shoot-the-test-scenario)): a whole test session
  from the computer (section 6).

## 1. Turning it on

```ini
[debug]
enabled = on
```

| Key | Type | Default | Effect |
|---|---|---|---|
| `enabled` | switch | `off` | the service; everything below needs it |
| `card_writes` | switch | `on` | `off` - `core` writes nothing to the card: no log file, no store saved, no module file written; all of it stays in RAM. The stores are still read at power-on. The next power-on then does not know how the last one ended, and changes made in the camera's OMKit menus are not kept across a power-off |
| `write` | switch | `off` | `on` - the computer may write files of the `OMKIT` folder (section 4), restart the camera and start or stop the test scenario (section 6) |
| `ring_level` | text | `debug` | how much of the log the RAM ring keeps: `error`, `warn`, `info`, `debug`. The card's log keeps `[core] log_level` as before |
| `tasks` | text | the card and recording tasks | firmware task names whose state each snapshot records, separated by commas, at most 11 (the `core` task is always added) |
| `window_ms` | number, ms | `1000` | 0..10000: after each write of `core` to the card, snapshots for this long ... |
| `window_every` | number, ms | `50` | 10..1000: ... this often (only changes are kept) |
| `stuck_ms` | number, ms | `1000` | the camera's application in a state that normally lasts a moment (half-press, a transition) for longer than this: snapshots every 500 ms, at most 20 |

The first lines of `CORE.LOG` (or of the RAM log) say what the service found:

| Log line | Meaning |
|---|---|
| `debug: on, RAM log bank B (power-on N) of 65536 bytes, level L` | the ring is ready |
| `debug: the run before (RUN, power-on N) did NOT end with a power-off; heartbeat ... ms, ...` | the RAM still holds the run that went down; its last log line follows |
| `debug: no RAM log of a run before` | the last run ended with a power-off or the battery was out: RAM was cleared |
| `debug: USB operations 9e00..9e02 ready (MTP mode)` | the computer can connect |

## 2. Security

Read this before you turn the service on.

- **With `enabled = on`, anyone who connects the camera by USB in the MTP mode can read** its RAM log, the state of
  its tasks, the camera's RAM (any address of its 256 MB of RAM, not the device registers) and the files in the
  `OMKIT` and `DCIM` folders of the card. No password or pairing is asked: USB is the trust boundary, as for the
  camera's own MTP access to your pictures.
- **With `write = on` as well, anyone connected can replace the files of the `OMKIT` folder**, including `CORE.BIN`,
  which runs at the next start, restart the camera, and start the test scenario, which presses the shutter button.
  Keep `write` off unless you update the camera this way yourself, and turn it off again afterwards.
- Nothing else is written: no other folder of the card, no camera memory, no firmware, no camera setting. The service
  runs no commands of the firmware's debug shell and changes no setting; it shoots only by the test scenario.
- Off (the default), the service registers nothing on USB and does not touch the RAM ring.

## 3. The computer: `tools/omkdebug.py`

Windows 10 or 11, Python 3.11 or later, and `pip install comtypes`. The camera's own driver is used (Windows Portable
Devices), so OM Workspace keeps working. Connect the cable, choose **MTP** in the camera's USB dialog (Storage,
MTP, Webcam, charging only, Exit), then:

```text
python tools/omkdebug.py hello                 the service, its flags and the power-on number
python tools/omkdebug.py tail --follow         this power-on's RAM log, as it grows
python tools/omkdebug.py postmortem            the RAM log of the power-on before (after a crash and restart)
python tools/omkdebug.py snap -o tasks.bin     every task of the three CPUs, now
python tools/omkdebug.py mem 0xa3470000 256    memory (RAM only)
python tools/omkdebug.py hist                  the camera's last 20 PTP operations
python tools/omkdebug.py level 4               the RAM log's level (until the next power-on)
python tools/omkdebug.py mark 1                a mark and a snapshot in the RAM log
python tools/omkdebug.py ls OMKIT\LOG          a folder of the card
python tools/omkdebug.py get OMKIT\LOG\CORE.LOG
python tools/omkdebug.py pictures -n 2 -o out  the last two pictures (JPEG and ORF)
python tools/omkdebug.py put build/CORE.BIN OMKIT\CORE.BIN      needs write = on
python tools/omkdebug.py info                  the build that runs (branch@commit), this run's name, the scenario
python tools/omkdebug.py restart               the camera restarts (write = on) and the tool waits for its new run
python tools/omkdebug.py shoot start "look:1, stock"   the test scenario: it waits until the cable is out
python tools/omkdebug.py shoot status
python tools/omkdebug.py session --help        a whole test session (section 6)
```

Windows lists the camera's pictures over MTP as soon as it connects; that reads the card, as always with MTP.

## 4. Writing files

`put` sends the file in pieces of 64 KB. The camera writes it to a temporary file `OMKDBG.TMP` in the target folder
(missing folders under `OMKIT` are made), then closes it, reads it back from the card and compares its size and CRC-32
with what the computer sent; only then the old file is removed and the new one renamed into its place. A difference,
a full card or a lost connection leaves the old file as it was (a temporary file without data for 30 s is removed).
While the camera writes a picture, the writes wait, like every other write of `core`. The new `CORE.BIN` and modules
run from the next power-on.

## 5. The protocol

Three vendor operations of PTP, registered at every USB connection in the MTP mode, without a PTP session; the first
parameter is the sub-command. All are answered at once: card work is a job of the `core` task, and the computer asks
for its state (`STATUS`) and then for its result.

| Operation | Sub-commands (`p1`) |
|---|---|
| `0x9E00` (no data) | 1 `HELLO` (p1 `OMKD`, p2 protocol 1, p3 core version, p4 flags: 64 `RESTART` works, 128 `SHOOT`, 256 `INFO`; p5 power-on), 2 `STATUS` (p1 job, state, number; p2 result; p3 bytes; p4 CRC-32; p5 extra), 3 `LEVEL`, 4 `MARK`, 5 `SNAP`, 0x10 `READ` (offset, length), 0x11 `LIST` (from), 0x20 `WBEGIN` (size, CRC-32), 0x22 `WCOMMIT`, 0x23 `WABORT`, 0x30 `RESTART` (p2 `RBT!`; the job ends, the request follows 500 ms later: p5 of `STATUS`), 0x31 `SHOOT` (p2 0 state - at once: p1 state and waiting << 8, p2 pictures planned, p3 made, p4 not seen made, p5 step and steps << 16; 1 start, 2 stop - jobs); 0x40-0x4F kept free for camera control |
| `0x9E01` (to the computer) | 1 `BANKS` (both RAM bank headers), 2 `BANK` (bank, position, length: raw records), 3 `MEM` (address, length up to 16 KB), 4 `HIST`, 5 `XFER` (offset, length: a job's result), 6 `INFO` (text, `key=value` lines: `core`, `build`, `run`, `power-on`, `restart`, `shoot`...) |
| `0x9E02` (from the computer) | 1 `PATH` (the card path of the next job), 2 `TEXT` (the steps of the next `SHOOT` start, up to 255 printable characters), 0x21 `WDATA` (offset: a piece of the file being written) |

Responses: `0x2001` OK, `0x2002` failed, `0x2005` no such sub-command, `0x200F` the write key is off, `0x2019` a job is
running, `0x201D` a parameter out of range. Job results (`STATUS` p2): 0 ok, 1 bad path, 2 cannot open, 3 card i/o, 4 size
differs, 5 CRC differs, 6 rename failed, 7 wrong state (a file open, a power-off or a restart under way), 8 timeout, 9
card, 10 restart not available (core 1.40-1.42), 11 a step not understood, 12 busy (a scenario runs or shoots).

The formats of the RAM banks, records and snapshots are described in `src/core/include/omk_dring.h` and `omk_debug.h`;
`tools/omkdebug.py` reads them all.

## 6. A test session

With `write = on`, a computer runs a whole test of a new build without touching the camera's buttons:

```text
python tools/omkdebug.py session --put build/CORE.BIN --put OMKIT.INI --scenario "look:Kodak Portra 400, stock" ^
    --restart --wait --pull-log --pull-pictures 2 -o run1
```

| Step | What happens | Checked |
|---|---|---|
| connect | waits for the camera in the MTP mode | the debug service answers; `write = on` when files are to be written |
| `--put` | each file into the `OMKIT` folder (a `.OMM` into `OMKIT\MODULES`; `FILE=OMKIT\PATH` for another place) | its CRC-32 read back from the card |
| `--scenario` | with `--restart`: the file `OMKIT\SHOOT.RUN` ([config.md](config.md#101-one-scenario-at-the-next-power-on-omkitshootrun)), which runs once at the next start (`--shots`, `--start-delay` go into it); without: the scenario starts now over USB | the steps are checked by the camera |
| `--restart` | the camera restarts and comes back | it left USB, then answers with a new run; the build that runs is the one of the `CORE.BIN` put |
| `--wait` | the scenario runs once the camera shoots: the tool asks you to take the cable out and to put it back when the pictures are made (or `--usb-off CMD --usb-on CMD` switch a hub's port off and on, for `--offline` seconds) | the camera comes back in the same run; the scenario's result |
| `--hands-free` | instead of `--restart` and `--wait`: the scenario runs with the cable in (below) | the result of this session in `OMKIT\SHOOT.END` |
| `--pull-log`, `--pull-pictures N` | the RAM logs of this power-on and of the one before (`ramlog.txt`, `ramlog_prev.txt`), `CORE.LOG` and the last N pictures into the output folder | each piece's CRC-32 |

Every step has a time limit (`--connect-timeout`, `--restart-timeout`, `--scenario-timeout`); the first step that fails
ends the session with its reason and the exit status 1.

**The restart.** `core` asks the camera's firmware to restart the way its own USB and Wi-Fi reboot requests do: the
firmware does an ordinary power-off - it saves the settings, finishes and unmounts the card, `core` does its last save as
for the power lever - and sets a wake-up alarm of one second, after which the camera starts from scratch: the new
`CORE.BIN`, modules and `OMKIT.INI` run. The firmware takes this request only while the camera is connected in the MTP
mode; elsewhere it ignores it and `core` logs `the reboot request was not taken`. `omkdebug.py restart --ptp` sends the
firmware's own request without `core`. Switching the camera off with its lever while the cable is in does **not** restart
it: with a charging source the camera only stops and charges, and the lever brings it back without a new start; take the
battery out for a cold start.

**The USB mode after the restart.** The camera comes back with the cable in, so it shows its USB dialog again: choose
MTP (the tool waits and reminds you). With the camera's USB mode set to MTP rather than to asking, it should connect
without the dialog (not tried yet). It does not take pictures in the MTP mode, which is why the cable has to go out for
the scenario - or the session goes without hands.

**Without hands.** `--hands-free` needs `--scenario` and a `core` that has it (`hello` lists the flag `hands-free`, or
put such a `CORE.BIN` in the same session):

```text
python tools/omkdebug.py session --put build/CORE.BIN --scenario keep --shots 3 --hands-free --pull-log ^
    --pull-pictures 3 -o run2
```

The tool writes `OMKIT\SHOOT.RUN` with `hands_free = on` and a session name ([config.md](config.md#102-without-hands-hands_free--on-in-shootrun)),
restarts the camera and waits. The camera starts again with the cable in but keeps it as charge only - it shoots and is
not on the computer -, runs the scenario, writes its result, restarts once more and comes back in the MTP mode; the
tool then reads the result, and `--pull-log` takes the RAM log of the scenario's run (`ramlog_prev.txt`, the power-on
before) with `CORE.LOG`. Leave the camera's USB mode at asking: `core` answers the dialog itself, with "charge only" for
the scenario and with MTP after it; with the mode set to MTP the camera connects before `core` could hold USB, and the
scenario ends at once (the tool says so). The tool's verdicts:

| What the tool says | What happened |
|---|---|
| `scenario: done: N of N pictures made` | all well; exit status 0 |
| `scenario: failed: ... (why)` | the scenario ended badly (`why` from the camera); the logs are pulled all the same |
| `the camera stayed on USB - the restart request was not taken` | the first restart did not happen |
| `the camera did not restart at the end` | `core` kept the camera on: it was connected to the computer before USB could be held |
| `the camera came up connected to the computer with the scenario waiting` | a `core` that did not hold USB; the scenario is stopped |
| `the camera is on USB but OMKit does not answer` | `core` does not run: the camera came on from charging by the cable without a full start (any step of any session says this after 15 s); switch it off with the lever and on again |
| `no camera in N s` | the camera hangs, stays off or stays in charge only: take the battery out and put it back, switch the camera on, then pull the log (the scenario's run is the one before) |
