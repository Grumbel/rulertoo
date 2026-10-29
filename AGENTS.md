# AGENTS.md — rulertoo

## Project
On-screen X11 ruler with real physical dimensions from XRandR.
Borderless, draggable, resizable, rotatable; units cm/mm/in/px.
Per-monitor calibration (Shift-C) when EDID sizes are wrong.

## Build
- `nix develop` / `nix build` / `nix run`
- In develop: `rulertoo-configure` | `rulertoo-build` | `rulertoo-run` | `rulertoo-run-gdb`
- CMake + Ninja
- Deps: libx11, libxrandr, libxrender, libxcb, xorgproto

## Version
- Top-level `VERSION` is the source of truth (see gamepkgstd VERSION-HANDLING)
- CMake defines `RULERTOO_VERSION`; flake passes `-DPROJECT_VERSION_FULL=…`

## Packaging / identity
- Reverse-DNS id: `org.grumbel.rulertoo`
- Desktop: `data/org.grumbel.rulertoo.desktop` (StartupWMClass=rulertoo)
- Icon: `data/icons/hicolor/scalable/apps/rulertoo.svg`
- AppStream: `data/metainfo/org.grumbel.rulertoo.metainfo.xml`
- Config: `$XDG_CONFIG_HOME/rulertoo/config` (position, unit, cal.<output>=…)

## Conventions
- GPLv3+
- REUSE / SPDX
- Author: Ingo Ruhnke <grumbel@gmail.com>
- Co-authored-by: {ai model} on agent commits
- Packaging norms: https://github.com/Grumbel/gamepkgstd (Linux desktop profile)

## Continuity
- See TODO.md
- Grok Web chat delivers changes as commits in a git bundle
- Claude can commit directly into the local repository

