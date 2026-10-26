# TODO — rulertoo

## Current tip
`38a87db` — Smooth resize + per-monitor calibration (Shift-C)

## Done
- [x] Functional ruler (XRandR scale, dual marks, free-end resize, units, …)
- [x] Exact integer mm↔px; -0 labels fixed
- [x] ARGB opaque text; high-contrast grip
- [x] Build clean: no unused-var warnings; libxcb for pkg-config; install docs
- [x] Desktop integration: .desktop, scalable SVG icon, AppStream metainfo, CMake install
- [x] Persist position/orientation/unit/length/height/alpha via XDG config
- [x] Smooth free-end resize (continuous length_mm, 1 px steps)
- [x] Per-monitor calibration (Shift-C, XDG config cal.<output>=factor)
- [x] flake: xorgproto (drop deprecated xorg.xorgproto)

## Open
- [ ] Full CRTC transform matrix
- [ ] XShape-only mode (re-add libXext when implemented)

## Notes
- Base for this work line: `9c013f1`
- Bundle contains the full stack from that base
