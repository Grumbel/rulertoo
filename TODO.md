# TODO — x11-ruler

## Current tip
`018568f` — Initial x11-ruler implementation

Bundle: `/home/workdir/artifacts/x11-ruler-001.1-initial-018568f.bundle`

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
- [ ] Improve alpha handling for marks on ARGB (ensure fully opaque)

## Notes for next agent
- Project source is under `/home/workdir/artifacts/x11-ruler`
- Bundle naming: x11-ruler-NNN.M-slug-baseshort.bundle
- Base of this line: `018568f` (fixed for the stack)
- Full compile/test not run in agent env (Nix/X11 limited); verify locally with `nix build` or cmake.
- To continue: `git pull /path/to/x11-ruler-001.1-initial-018568f.bundle HEAD`
