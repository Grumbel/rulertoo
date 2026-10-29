# TODO — rulertoo

## Current tip
`41e917f` — flake: libxau (drop deprecated xorg.libXau)

## Done
- [x] Functional ruler (XRandR scale, dual marks, free-end resize, units, …)
- [x] Exact integer mm↔px; -0 labels fixed
- [x] ARGB opaque text; high-contrast grip
- [x] Build clean: no unused-var warnings; libxcb/libxau for pkg-config; install docs
- [x] Desktop integration: .desktop, scalable SVG icon, AppStream metainfo, CMake install
- [x] Persist position/orientation/unit/length/height/alpha via XDG config
- [x] Smooth free-end resize (continuous length_mm, 1 px steps)
- [x] Per-monitor calibration (Shift-C; Ctrl-C cycles 5–30 cm / 2–12 in)
- [x] Separate horizontal / vertical calibration (cal.OUTPUT=W,H)
- [x] flake: xorgproto, libxau (drop deprecated xorg.* names)
- [x] gamepkgstd (Linux desktop): VERSION, --version, WM_CLASS, develop helpers
- [x] Colour schemes on `c` (default / opaque / inverted / high-contrast / amber)
- [x] Clamp position on rotate/place so vertical rulers are not cut off
- [x] Blank off-monitor segments (outline only) when spanning outputs
- [x] Live multi-monitor blanking while moving; override-redirect vs panel
- [x] Man page: man/rulertoo.1 installed to mandir/man1
- [x] Silence xau pkg-config noise, unused-function warnings, CMake unused-CLI

## Open
- [ ] Full CRTC transform matrix
- [ ] XShape-only mode (re-add libXext when implemented)

## Notes
- Base for this work line: `9c013f1`
