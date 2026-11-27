# CREATPRF: colour and monochrome profiles

CREATPRF (version 1.6, needs core API 20) gives the camera three **colour profiles** and three **monochrome profiles**:
picture modes that are chosen in the picture mode screen, edited there with the camera's own editor column, and applied
to stills, live view and video recorded from the still modes. The camera's firmware has the settings of these profiles
but does not offer them; CREATPRF shows them and applies them, without changing the camera's stored settings to anything
the stock firmware does not know.

## 1. Getting started

With an OMKit release (or the card bundle, `make -C OMKit card`) there is nothing to do: the card's `OMKIT` folder has
`MODULES\CREATPRF.OMM`, and its `OMKIT.INI` already has `CREATPRF = on`. Switch the camera on and the picture mode strip
shows six more entries.

On a card put together by hand:

1. Copy `CREATPRF.OMM` to `OMKIT\MODULES\` on the card.
2. In `OMKIT.INI` ([../../config.md](../../config.md)) add it to `[modules]`:

   ```ini
   [modules]
   enabled = on
   CREATPRF = on
   ```

3. Switch the camera off and on. The picture mode strip now has six more entries.

## 2. On the camera

### 2.1. Choosing a profile

- The strip lists, after the camera's stock modes and before the ART filters, "Color Profile 1" to "3" and "Mono Profile
  1" to "3", each with its own icons (strip, large icon, the "Picture Mode" menu line, the control panel cell, the
  shooting screen sign): a colour profile is "C1" to "C3" in a frame, a monochrome one "M1" to "M3" on a solid plate.
- Choosing a profile turns it on; choosing any other mode turns it off. A profile and the FILMLOOK look exclude each
  other.
- While a profile is on, the camera stores Natural as its picture mode; the profile is applied from memory. Settings
  files and custom modes see Natural.
- The chosen profile is remembered and brought back at the next power-on, unless the stored picture mode is no longer
  Natural by then (it was changed without OMKit): then the profile is forgotten.
- The control panel cell shows the active profile, but its dial does not step through the profiles: they are chosen in
  the picture mode screen.

### 2.2. Editing a profile

The column of a profile is the camera's own editor column; its values are the profile's own settings, stored by the
camera like any other setting.

| Profile | Items |
|---|---|
| colour | Color/Vivid (the saturation of 12 hue sectors, -5..+5; INFO switches between one hue and all hues; long OK sets all to 0), tone curve (highlights, mid tones, shadows, -7..+7), sharpness and contrast (-2..+2) |
| monochrome | Color/Level (the colour filter: none, yellow, orange, red, magenta, blue, cyan, green, green-yellow, and its level), tone curve, shading (-5..+5), sharpness, contrast, grain (Off, Low, Med, High), toning |

- The graphic editors (Color/Vivid, Color/Level, the tone curve) are drawn by OMKit, over live view; this firmware has
  their dial logic but no drawing of its own ([../../platform/om5m2/ui.md](../../platform/om5m2/ui.md#43-what-omkit-draws)).
- The tone curve: the rear dial edits the highlights, the front dial the shadows; INFO switches both dials to the mid
  tones and back; long OK sets all three to 0. Its dial labels stand as on the camera's Highlight & Shadow screen: "Sh" on
  the left, over the shadows of the graph, "Hi" on the right, each with the pictogram of the dial that edits it; after
  INFO both read "Mid" (core 1.28; before it "Hi" stood on the left and "Sh" on the right).
- Color/Level: the dial labels read "Color" (the filter, the camera's own label) and "Level"; every dial label is in the
  camera's own face and size and stands at the same place under its dial pictogram (core 1.29 and CREATPRF 1.5; before
  them the left one read "Color Filter" in a wider field, the right one a smaller "Level").
- The shading item of the colour profiles is removed from their column: the camera does not apply a colour profile's
  shading.
- The camera's Highlight & Shadow screen and the control panel's Highlight & Shadow cell edit and show the active
  profile's tone curve while a profile is on; in a stock mode they work as before.
- The dials are the only controls: the camera has no touch handling for these editors.

### 2.3. What the pictures get

- Stills: the profile's values; the picture's maker data names it (Color Profile n or Monochrome Profile n).
- Live view: the profile's values, at once after a change (a change between colour and monochrome may show one frame of
  Natural).
- Video recorded from a still mode carries the profile.
- Not applied (the picture is Natural): ART bracketing frames, in-camera RAW development, and while the stored mode is
  not Natural. A monochrome still taken while the camera's image memory is not in the layout a monochrome picture needs
  is taken as Natural instead.

### 2.4. Loading profiles from a computer

OM Workspace's "Load Color Profile" and "Load Monochrome Profile" over USB work with CREATPRF running: the loaded values
go into the profile's settings, and a loaded "profile n" line chooses that profile as the user would. Lines for custom
mode slots are skipped (profiles are not kept in custom modes). Details:
[../../platform/om5m2/picture-modes.md](../../platform/om5m2/picture-modes.md#87-loading-profiles-from-om-workspace).

## 3. Files

CREATPRF reads no files of its own: it has no `OMKIT\CREATPRF\` folder (`core` makes a module's folder only when the
module first opens or lists a file). Its 41 pictures - the icons of the six profiles and the pictures of its editors -
are inside `CREATPRF.OMM` and are read from it at power-on.

| File | Contents |
|---|---|
| `OMKIT\MODULES\CREATPRF.OMM` | the module, with its pictures |
| `OMKIT\STORE\CREATPRF.STA`, `.STB` | its store: which profile is active (two copies, written 1.5 s after a change and at power-off) |

The profiles' settings themselves are the camera's own settings, not in these files.

## 4. Settings

Section `[CREATPRF]` in `OMKIT.INI`:

| Key | Default | Values | Effect |
|---|---|---|---|
| `mono_filters` | `original` | `original`, `alternative` | `alternative` - softer colour filters for the monochrome profiles: for RED, MAGENTA, BLUE and CYAN, level 3 becomes the camera's level 2, and level 2 lies halfway between the camera's levels 1 and 2. The other filters and level 1 stay. `original` - the camera's own filters. Any case (`Alternative`); another value - a warning and `original` |

The alternative matrices are computed at power-on from the camera's own ones (each row keeps the sum the camera's rows
have, so grey stays grey); no camera data is stored in the module. A filter whose matrices cannot be read keeps the
camera's own (a warning in the log).

`core` also reads `memory` and `event_<event>` in this section: [../../config.md](../../config.md#8-keys-core-reads-in-every-modules-section).

## 5. Log lines

| Line | Meaning |
|---|---|
| `CREATPRF: colour profile N chosen`, `monochrome profile N chosen`, `stock mode 0 chosen` | the user chose a profile or left it |
| `CREATPRF: colour profile N restored` | brought back at power-on |
| `CREATPRF: colour profile N not restored, code -4` | the stored picture mode was not Natural; the profile is forgotten |
| `CREATPRF: monochrome filters: alternative, N of 8 matrices set` | `mono_filters = alternative` took effect |
| `CREATPRF: mono_filters = X: not original or alternative - original` | an unknown `mono_filters` value |
| `CREATPRF: picture profiles not available, code N` | the profile service could not start (for example another module owns it): the module stops |
| `CREATPRF: this camera has no picture profiles support, nothing to do` | the module stops |

More detail (pictures set, the matrices) with `log_level = debug` in `[core]`, in a debug build of `core` and of the
module only (`BUILD=debug`).

## 6. Technical details

- The module uses `core`'s profile service (`prof_*`, API 6; the filter matrices API 18): [../../sdk.md](../../sdk.md).
  `core` and the driver keep the profile in the camera's interface, shooting and live view; the module chooses, restores
  and provides pictures.
- How the camera's profiles work and what OMKit changes:
  [../../platform/om5m2/picture-modes.md](../../platform/om5m2/picture-modes.md#8-omkit-creatprf); the editors and their
  pictures: [../../platform/om5m2/ui.md](../../platform/om5m2/ui.md).
- Source: `src/modules/creatprf/` (`creatprf.c`, `filters.c`); the pictures are made by `tools/mkicons_creatprf.py` and
  kept in `resources/modules/creatprf/icons/`.
