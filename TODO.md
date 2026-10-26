# TODO — rulertoo

## Current tip
Pending commit (rename + features).

## Done
- [x] XRandR physical sizing, per-monitor
- [x] Classic ruler marks + numbers
- [x] ARGB bg, opaque marks via XRender
- [x] Drag to move; edge handles to resize
- [x] Rotate (button + key + right-click on handle)
- [x] Middle-drag numbering origin
- [x] Units: cm / mm / in / px
- [x] CLI: --length --height --alpha --unit --vertical
- [x] Flicker reduction (throttled monitor query, coalesced motion)
- [x] Rename project to rulertoo; fix nixpkgs xorg deprecations

## Open
- [ ] Smoother resize feedback / live length readout
- [ ] Persist position/orientation/unit
- [ ] Better vertical number layout
- [ ] XShape-only mode without compositor
- [ ] Fractional inch marks

## Notes
- Base of work line: 018568f
- Source: /home/workdir/artifacts/rulertoo
