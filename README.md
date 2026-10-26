# x11-ruler

A simple borderless on-screen ruler for X11 that uses **real physical
dimensions** reported by XRandR (not the unreliable global DPI).

## Features

- Classic ruler marks: short every millimetre, longer every 5 mm, full
  height + numbers every centimetre.
- Automatically detects which monitor the window is on and scales the
  pixel length so that the ruler measures true centimetres on that
  panel.
- Borderless, utility window; drag with the left mouse button.
- Semi-transparent background when an ARGB visual and a compositor are
  available; otherwise a solid light background.
- Quit with Escape, `q`, or right-click.

Default length is 30 cm. The window height is fixed in pixels; only the
width is derived from the monitor’s physical size.

## Build

### Nix

```bash
nix build
./result/bin/x11-ruler
# or
nix run
```

### CMake

```bash
cmake -B build -G Ninja
cmake --build build
./build/x11-ruler
```

Dependencies: `libX11`, `libXrandr`, `libXext`, `libXrender`,
`pkg-config`, CMake ≥ 3.16, a C++17 compiler.

## Licence

GPL-3.0-or-later. See the SPDX headers and the REUSE configuration.
