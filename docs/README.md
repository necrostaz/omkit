# OMKit documentation

For camera owners:

| Document | Contents |
|---|---|
| [platform/om5m2/fw1100/install.md](platform/om5m2/fw1100/install.md) | Installing OMKit on the OM-5 Mark II with firmware 1.1: the update files, the loader patch, the card, first start, going back to stock, warnings, troubleshooting |
| [config.md](config.md) | `OMKIT.INI`: every setting of `core`, the file's rules, the sample `core` writes, pointers to the module settings |
| [modules/](modules/README.md) | the bundled modules: [FILMLOOK](modules/filmlook/README.md) (film looks, with its [presets](modules/filmlook/presets.md) and [grain tiles](modules/filmlook/grain.md)), [CREATPRF](modules/creatprf/README.md) (colour and monochrome profiles), [HELLO](modules/hello/README.md) (the example module) |
| [apps/](apps/README.md) | programs for the computer: the [ISP player](apps/isp_player.md) shows a picture through an `.isp` profile as the camera would render it; the [ISP fitter](apps/isp_fitter.md) makes an `.isp` profile from a `.cube` look; [ISP neurofit](apps/isp_neurofit.md) makes one from the camera's RAW files and scans of a film, with an AI agent judging by eye |

For developers:

| Document | Contents |
|---|---|
| [build.md](build.md) | Building from source: requirements and packages, the MN10300 toolchain, `core`, modules and the card bundle, outputs and variables, pictures and data sets, `make test`, troubleshooting, the source tree |
| [release.md](release.md) | Building a release ZIP: layout, `patch.json`, the builder's checks, adding a platform or firmware |
| [architecture.md](architecture.md) | How OMKit works, from the loader patch to the modules: layers, files on the card, boot sequence, tasks and CPUs, hooks, events, services, log, store, memory, power-off and sleep, failure handling, source layout, porting |
| [modules.md](modules.md) | What a module is, its lifecycle, the safety, correctness and performance rules every module follows, and a guide to a first module |
| [sdk.md](sdk.md) | The module API and SDK: headers, calling contexts, API levels and compatibility, every function with its return codes, events, build tools |
| [formats/isp.md](formats/isp.md) | The `.isp` picture profile file |
| [formats/grn.md](formats/grn.md) | The `.GRN` grain tile container |
| [platform/om5m2/](platform/om5m2/README.md) | Knowledge base of the first platform, the OM-5 Mark II: hardware, RTOS, memory, power, storage, firmware, picture modes, UI, ISP colour path, still pipeline, grain; [fw1100/](platform/om5m2/fw1100/README.md) - firmware 1.1: identity, the loader patch, address catalogues |
