# TODO — rulertoo

## Current tip
`42d5caf` — Readable labels + CRTC rotation-aware physical scale

Bundle: `/home/workdir/artifacts/rulertoo-005.1-contrast-rotation-018568f.bundle`

## Done
- [x] XRandR physical sizing, per-monitor
- [x] Classic ruler marks + numbers
- [x] ARGB bg, opaque marks via XRender
- [x] Drag to move; edge handles to resize
- [x] Rotate (button + key + right-click on handle)
- [x] Middle-drag numbering origin
- [x] Units: cm / mm / in / px
- [x] CLI options
- [x] Flicker reduction
- [x] Rename to rulertoo; libx11 package names
- [x] Build fix: DragMode::None → Idle
- [x] High-contrast outlined numbers/labels
- [x] CRTC rotation: map physical mm to screen axes

## Open
- [ ] Live length readout while resizing
- [ ] Persist position/orientation/unit
- [ ] Full CRTC transform matrix (non-uniform scale / reflect)
- [ ] XShape-only mode without compositor
- [ ] Fractional inch marks

## Notes
- Base of work line: `018568f`
- Source: `/home/workdir/artifacts/rulertoo`
