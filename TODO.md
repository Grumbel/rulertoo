# TODO — rulertoo

## Current tip
`ec0c280` — Desktop integration (.desktop, icon, AppStream)

## Done
- [x] Functional ruler (XRandR scale, dual marks, free-end resize, units, …)
- [x] Exact integer mm↔px; -0 labels fixed
- [x] ARGB opaque text; high-contrast grip
- [x] Build clean: no unused-var warnings; libxcb for pkg-config; install docs
- [x] Desktop integration: .desktop, scalable SVG icon, AppStream metainfo, CMake install

## Open
- [ ] Persist position/orientation/unit
- [ ] Optional calibration when EDID mm is wrong
- [ ] Full CRTC transform matrix
- [ ] XShape-only mode (re-add libXext when implemented)

## Notes
- Base for this work line: `9c013f1`
- Source / bundles under artifacts
