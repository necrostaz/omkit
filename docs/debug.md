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
  one checked by a CRC-32 read back from the card before it replaces the old file.

## 1. Turning it on

```ini
[debug]
enabled = on
```

| Key | Type | Default | Effect |
|---|---|---|---|
| `enabled` | switch | `off` | the service; everything below needs it |
| `card_writes` | switch | `on` | `off` - `core` writes nothing to the card: no log file, no store saved, no module file written; all of it stays in RAM. The stores are still read at power-on. The next power-on then does not know how the last one ended, and changes made in the camera's OMKit menus are not kept across a power-off |
| `write` | switch | `off` | `on` - the computer may write files of the `OMKIT` folder (section 4) |
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
  which runs at the next start. Keep `write` off unless you update the camera this way yourself, and turn it off
  again afterwards.
- Nothing else is written: no other folder of the card, no camera memory, no firmware, no camera setting. The service
  runs no commands of the firmware's debug shell and has no way to shoot or change settings.
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
python tools/omkdebug.py restart               not available yet: asks you to switch the camera off and on, then
                                               waits for it and checks it is a new power-on
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
| `0x9E00` (no data) | 1 `HELLO` (p1 `OMKD`, p2 protocol 1, p3 core version, p4 flags, p5 power-on), 2 `STATUS` (p1 job, state, number; p2 result; p3 bytes; p4 CRC-32; p5 extra), 3 `LEVEL`, 4 `MARK`, 5 `SNAP`, 0x10 `READ` (offset, length), 0x11 `LIST` (from), 0x20 `WBEGIN` (size, CRC-32), 0x22 `WCOMMIT`, 0x23 `WABORT`, 0x30 `RESTART` (not available: the job fails); 0x40-0x4F kept free for camera control |
| `0x9E01` (to the computer) | 1 `BANKS` (both RAM bank headers), 2 `BANK` (bank, position, length: raw records), 3 `MEM` (address, length up to 16 KB), 4 `HIST`, 5 `XFER` (offset, length: a job's result) |
| `0x9E02` (from the computer) | 1 `PATH` (the card path of the next job), 0x21 `WDATA` (offset: a piece of the file being written) |

Responses: `0x2001` OK, `0x2002` failed, `0x2005` no such sub-command, `0x200F` the write key is off, `0x2019` a job is
running, `0x201D` a parameter out of range. The formats of the RAM banks, records and snapshots are described in
`src/core/include/omk_dring.h` and `omk_debug.h`; `tools/omkdebug.py` reads them all.
