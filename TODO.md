# TODO — rulertoo

## Current tip
`7ba4e4d` — Persist position/orientation/unit via XDG config

## Done
- [x] Functional ruler (XRandR scale, dual marks, free-end resize, units, …)
- [x] Exact integer mm↔px; -0 labels fixed
- [x] ARGB opaque text; high-contrast grip
- [x] Build clean: no unused-var warnings; libxcb for pkg-config; install docs
- [x] Desktop integration: .desktop, scalable SVG icon, AppStream metainfo, CMake install
- [x] Persist position/orientation/unit/length/height/alpha via XDG config

## Open
- [ ] Optional calibration when EDID mm is wrong
- [ ] Full CRTC transform matrix
- [ ] XShape-only mode (re-add libXext when implemented)

## Notes
- Base for this work line: `9c013f1`
- Previous tip (desktop): `80914d5`
- Bundle under artifacts
