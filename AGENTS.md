# AGENTS.md — x11-ruler

## Project
Simple X11 on-screen ruler that shows real physical dimensions (mm/cm)
based on XRandR monitor geometry. Borderless, draggable, auto-resizes
to the monitor it currently occupies.

## Build
- `nix develop` or `nix build`
- CMake + Ninja (or make)
- Dependencies: X11, Xrandr, Xext (shape), Xrender

## Conventions
- GPLv3+
- REUSE compliant, SPDX headers
- Author: Ingo Ruhnke <grumbel@gmail.com>
- Co-authored-by: Grok <grok@x.ai> on agent commits

## Continuity
- See TODO.md for current tip and open work.
- Deliverables are git bundles only; never patches.
