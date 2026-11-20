# OMKit modules

The modules that come with OMKit. Each is a file `OMKIT\MODULES\NAME.OMM` on the card, loaded when `[modules]` of
`OMKIT.INI` has `NAME = on` ([../config.md](../config.md#4-modules)). A release and the card bundle carry an
`OMKIT.INI` in which the modules of the card bundle are already on. How modules work and how to write one:
[../modules.md](../modules.md).

| Module | Version | In the card bundle (on) | What it does |
|---|---|---|---|
| [FILMLOOK](filmlook/README.md) | 1.12 | yes, with 32 [presets](filmlook/presets.md) and 11 [grain tiles](filmlook/grain.md) | film looks in place of a stock picture mode: `.ISP` presets chosen with the dials, an editable tone curve, film grain on the stills |
| [CREATPRF](creatprf/README.md) | 1.5 | yes | three colour and three monochrome profiles in the picture mode screen, edited with the camera's own editor column |
| [HELLO](hello/README.md) | 1.0 | no | the example module for developers: exercises the loader and the basic API |
