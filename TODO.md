# TODO — rulertoo

## Current tip
`87191c4` — Fix DragMode::None / X11 macro clash

Bundle: `/home/workdir/artifacts/rulertoo-004.1-fix-none-macro-018568f.bundle`

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
- [x] Build fix: DragMode::None → Idle (X11 None macro)

## Open
- [ ] Smoother resize feedback / live length readout
- [ ] Persist position/orientation/unit
- [ ] Better vertical number layout
- [ ] XShape-only mode without compositor
- [ ] Fractional inch marks

## Notes
- Base of work line: `018568f` (fixed)
- Source: `/home/workdir/artifacts/rulertoo`
