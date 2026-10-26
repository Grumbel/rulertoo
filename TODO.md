# TODO — x11-ruler

## Current tip
Initial implementation (this bundle).

## Done
- [x] Basic X11 window, borderless, draggable
- [x] XRandR monitor detection + physical mm size
- [x] Auto pixel size from physical length (default 30 cm)
- [x] Classic ruler marks: mm short, 5 mm medium, cm long + numbers
- [x] ARGB transparency when available (half-transparent bg + opaque marks)
- [x] CMakeLists.txt + flake.nix
- [x] REUSE / SPDX / GPLv3

## Open / later
- [ ] Vertical orientation / rotation
- [ ] Mouse wheel or keys to change length
- [ ] Persist position
- [ ] Multi-monitor edge cases (spanning)
- [ ] Fallback when no ARGB / no compositor
- [ ] XShape-only mode for non-compositing WMs

## Notes for next agent
- Project lives under /home/workdir/artifacts/x11-ruler (persistent).
- Bundle naming: x11-ruler-NNN.M-slug-baseshort.bundle
- Base of this line is the initial root commit short hash.
- Full build/test not completed in the agent environment; hand off for local verification.
