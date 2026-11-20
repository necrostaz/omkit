# OM-5 Mark II: power-off, sleep and the power lever

How the camera switches off and goes to sleep, what it waits for, and what OMKit must have done before the card or the
power goes away. Function addresses and hook sites for firmware 1.1 are in
[fw1100/system.md](fw1100/system.md#5-power-and-sleep).

Basis labels: [README.md](README.md#basis-labels).

## 1. Summary

| Question | Answer | Basis |
|---|---|---|
| Who switches the camera off | One task, the camera application, step by step. It sends requests to servers (file system, power task) and waits for each reply without a timeout | code, emulator |
| Does it wait for other tasks | No. There is no "stop all tasks" step; `core` keeps running during power-off | code, emulator |
| When is the card gone | Near the end: the application asks the file system server to switch the card off, after a check that the lever has not been turned on again | code, emulator, camera |
| How long | From the lever to the card being switched off: 2.8-5 s on the camera; the whole power-off takes about 3-4 s with or without OMKit | camera |
| Cancel | Lever back on within that time: power-off is cancelled, nothing restarts, everything (including `core`) keeps running | camera |
| Sleep | Light sleep freezes every task and powers the card down **without** unmounting it; deep sleep is a real power-off with an alarm | code, camera |
| Settings | The camera writes its settings to flash itself during power-off and before sleep, before OMKit is told in some paths | code, emulator |

## 2. Power-off

### 2.1. The chain

1. **The lever.** A key input event becomes the system event "power switch off". The firmware sets a "lever off accepted"
   word at once (camera: this word is the earliest sign, 2-3 s before the card goes away).
2. **The application.** Its state machine enters "power off" and calls `CameraCommon::shutDown(mode, reason)`. Almost
   every power-off path goes through this function: lever, deep sleep, battery, Quick Sleep, interval shooting, reboot.
   Reasons: 1 lever, 3 USB, 4 deep sleep (auto power off), 5 battery, 6 forced, 7 interval shooting, 10 Quick Sleep,
   11 reboot (code).
3. **`OlyHfwPower::startShutDown`.** Writes the settings to flash (section 4), waits while certain keys are still pressed
   (20 ms steps), reads the lever and USB inputs, then calls the system power-off service.
4. **The last stage** (all in the application task):

| Step | What | May wait |
|---|---|---|
| a | Subprocessor command | polls a register |
| b | If the firmware's debug log is on, it is written to the card | card write |
| c | Waits while Bluetooth LE is busy, then 5 x 100 ms | **no limit** while BLE is busy |
| d | Lever (or another wake key) on again? Then cancel | no |
| e | **Card off**: request 53 to the file system server, synchronous | **no limit**: the server finishes everything queued before it |
| f | Wake keys checked again | no |
| g-j | Messages to the power task (switch parts off) and to other servers, each waiting for a reply | **no limit** |
| k | Alarm in the subprocessor if a wake-up time is set (deep sleep, interval shooting) | no |
| l-m | Power-off mode, last command to the subprocessor; CPU 0 spins until power goes | no |

Some paths call the last stage directly, bypassing `shutDown` (an update-related path among them); the card-off step (e)
is common to all of them (code).

### 2.2. What must happen before power goes away

- **Before the card is switched off (step e)**: everything that must reach the card is written and every file is closed.
  After step e, `fopen` returns 0 and writes do not happen (emulator). A file call that is queued in the file system
  server before request 53 is still served; the next one is not.
- **No file call may be in progress at step e** if it can be avoided: OMKit holds the card-off step until `core` confirms
  that it has finished writing, for at most 1 s, then lets the firmware go on in any case.
- **Nothing is written after the confirmation.** `core` stops drawing, stops timers and module work, and never opens a file
  again in that power-off.
- **Do not use `fflush`.** With a `core` build that called it after writing its store, the camera did not switch off
  (camera); that `fflush` was the cause is an assumption, but nothing needs it: data reach the card on `fclose` anyway.
- **Do not rely on the power-off event to clean the camera's settings** (section 4).

### 2.3. How `core` handles it

`core` has three states: running, powering off, asleep. Hooks in the firmware change the state (hook sites:
[fw1100/system.md](fw1100/system.md#5-power-and-sleep)):

| Hook | Effect |
|---|---|
| `CameraCommon::shutDown` entry | State "powering off", reason from the call |
| Card-off step | If no signal came yet, this is it (paths that bypass `shutDown`). Waits up to 1 s for `core`'s confirmation, then writes the last log lines itself |
| `CameraCommon::startSleep` entry | State "asleep" |
| Entry of the sleep service | Waits up to 1 s for `core`'s confirmation before the firmware powers the card down |
| Return from sleep | State "running", the sleep result is logged |

Plus two signals without hooks:

- **The lever word**, polled every 250 ms: on the camera it changes 2-3 s before `shutDown` runs, so in practice it is the
  first signal of a lever power-off (`power lever flag (polled)` in the log).
- **The lever word at start**: `core` starts about 1.4 s after power-on. If the lever was already turned off by then,
  `shutDown` ran before the hooks were installed. `core` checks the word right after installing the hooks: if it is set,
  modules are not loaded and nothing is drawn; if the camera is still alive after 5 s, it was a cancel and `core` loads the
  modules then (camera).

Each state change gets a sequence number; the `core` task does its work for the new state and confirms the number. The
hooks wait for the confirmation in 10 ms steps, at most 1 s. On the camera `core` was always ready in time (`waited 0 ms`).

| State | What `core` does |
|---|---|
| Powering off | Modules get `OMK_POWER_OFF` (last loaded first); then the log and all stores are written once; nothing after that |
| Cancelled (camera alive 5 s after the signal) | Back to running; modules get `OMK_POWER_RESUME`; the card may be unavailable for a while (writes retry) |
| Asleep | Modules get `OMK_POWER_SLEEP`; card writes stop, the log waits in memory |
| Running again | Modules get `OMK_POWER_WAKE`; writing resumes. If neither sleep nor wake-up happened within 15 s, `core` returns to running by itself |

Module handlers are short and must not sleep; while the state is not "running", the file API of modules refuses and
deferred module work does not run. The API is described in [../../sdk.md](../../sdk.md).

## 3. Sleep

### 3.1. Stages

| Stage | When | What it is | Basis |
|---|---|---|---|
| Backlight off | `Backlit LCD` time | Events only | code |
| Light sleep | `Sleep` time (1/3/5 min of inactivity) | CPU 0 spins in a loop in SRAM with interrupts off, CPUs 1 and 2 are stopped, every task is frozen where it was. DRAM is kept. At most 5 minutes | code, camera |
| Deep sleep | Light sleep ran out | A power-off with reason 4 and an alarm in the subprocessor. Waking up is a new boot | code |
| Auto Power Off | Setting 5 min: right after 5 min of light sleep; 30/60/240 min: by the alarm of deep sleep (assumption) | Power-off | code |
| Quick Sleep | Its own short timer | Deep sleep at once, reason 10, no light sleep | code |

### 3.2. What happens to the card in light sleep

Before sleeping, the application waits up to 10 s while two "media busy" words show that the camera itself is using the
card (code). The sleep code then **cuts the card's power without unmounting it** and without asking the file system
server; on wake-up the power comes back before tasks continue, and the file system carries on (code). A file request that
the server is executing at that moment could be cut off (assumption). Hence `core` stops card writes on the sleep signal
and the sleep service hook waits for its confirmation.

### 3.3. After waking

- Tasks continue from where they were; a `dly_tsk` in progress returns after the wake-up (inference).
- The wake-up reason: key, battery, timeout, USB, card. A timeout leads to deep sleep, that is to a power-off (code).
- Time: the subprocessor clock keeps counting; the microsecond timer and kernel time may not (see
  [hardware.md](hardware.md#7-time)). On the camera, one sleep showed 0.59 s on the microsecond timer; its real length was not measured.
- After deep sleep the camera boots again, and the OMKit loader runs as on a normal power-on: `core` starts (camera).

## 4. Settings in flash during power-off and sleep

The camera keeps its settings in RAM and writes the whole settings image to flash only at certain moments: when a menu is
closed (if something changed), at every power-off (in `startShutDown`, **before** the card is switched off), before
sleeping, before a firmware update from a running camera, at a factory reset, and in its crash handler (code, emulator).
There is no periodic save: taking the battery out keeps the state of the last save.

OMKit's power-off event does **not** reliably come before that write. In the emulator the `core` task (lowest priority)
got the CPU only after the application had saved the settings. On the camera the lever word comes early, but deep sleep,
Quick Sleep, battery, sleep and menu closing give no early sign. Consequence: whatever a module leaves in the camera's
settings may be in flash at any moment, so modules must never put values there that the stock firmware cannot handle
(see [storage.md](storage.md#5-camera-settings-in-flash)).

## 5. Card write ordering (summary)

1. While the camera itself uses the card (a picture being written, its own media work), `core` postpones its writes; it
   also waits 3 s after shooting parameters were built. The log shows `card: core writes waited N ms`.
2. Files are written by appending only the new part and closed at once (`ab`, write, close); nothing stays open across
   service ticks.
3. On a power-off signal: one last write of the log and the stores, all files closed, confirmation to the card-off hook.
4. On a sleep signal: no writes until wake-up.
5. After a cancelled power-off the card may already be off: the first write after it checks the result of `fopen`, and the
   stores retry every 1.5 s (`store ... cannot open`, then `card back, saving`).

## 6. Open points

- What exactly the power task does with its messages, and where the panel goes dark.
- Why a store write with `fflush` at one moment prevented power-off while the same write at another moment did not.
- Whether the lever word is cleared on a cold start in every case (it is 0 in every start observed).
- One power-off on the camera left no last log lines (the card-off hook did not run or `core` did not confirm in time); it
  did not happen again in 15 later power-offs.
