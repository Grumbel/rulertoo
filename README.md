# rulertoo

Borderless on-screen ruler for X11 that uses **real physical dimensions**
from XRandR (not global DPI).

## Features

- Classic marks: mm / 5 mm / cm
- Scales to the monitor it sits on (XRandR mm sizes)
- Horizontal or vertical
- Resize by dragging the free-end handle (right or bottom)
- Shift numbering origin with middle-button drag
- Units: cm, mm, inch, pixel (cycle with `u`)
- Semi-transparent background; marks drawn fully opaque
- CLI options for length, height, alpha, unit, orientation

## Controls

| Input | Action |
|-------|--------|
| Left-drag body | Move |
| Drag free end | Resize length |
| Middle-drag | Shift zero / numbering origin |
| Click **R** button or `r` | Rotate |
| `u` | Cycle unit |
| Right-click / Esc / `q` | Quit |

## Build

```bash
nix build && ./result/bin/rulertoo
# or
cmake -B build -G Ninja && cmake --build build && ./build/rulertoo
```

## Licence

GPL-3.0-or-later
