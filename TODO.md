# TODO — rulertoo

## Current tip
`4550a1e` — gamepkgstd Linux desktop alignment

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
- [x] gamepkgstd (Linux desktop): VERSION file, CMake/Nix version wire-up, --version,
      WM_CLASS + StartupWMClass, nix develop helpers, packaging identity docs

## Open
- [ ] Full CRTC transform matrix
- [ ] XShape-only mode (re-add libXext when implemented)

## Notes
- Base for this work line: `9c013f1`
- Packaging reference: git@github.com:Grumbel/gamepkgstd.git (Linux desktop profile only;
  Windows/Android/WASM/PortMaster/game UI are N/A for this X11 utility)
