// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

#ifndef RULERTOO_VERSION
#define RULERTOO_VERSION "unknown"
#endif

constexpr int kDefaultLengthMm = 300;
constexpr int kDefaultHeightPx = 52;
constexpr double kDefaultBgAlpha = 0.40;
constexpr int kHandlePx = 10;          // free-end (right/bottom) resize handle
constexpr int kButtonPx = 18;          // R (rotate) / F (flip) button size
constexpr int kButtonGap = 4;          // space between the two buttons
// Calibration reference lengths (metric then imperial). Ctrl-C cycles while
// calibrating; Shift-C starts at the 30 cm entry.
struct CalTarget {
  double mm;
  const char* label;  // short banner text, e.g. "30 cm"
};
constexpr CalTarget kCalTargets[] = {
  { 50.0,        "5 cm"  },
  { 100.0,       "10 cm" },
  { 200.0,       "20 cm" },
  { 300.0,       "30 cm" },
  { 2.0 * 25.4,  "2 in"  },
  { 4.0 * 25.4,  "4 in"  },
  { 8.0 * 25.4,  "8 in"  },
  { 12.0 * 25.4, "12 in" },
};
constexpr int kCalTargetCount = static_cast<int>(sizeof(kCalTargets) / sizeof(kCalTargets[0]));
constexpr int kCalDefaultIndex = 3;  // 30 cm

enum class Unit { Mm, Cm, Inch, Pixel };

// Visual themes cycled with `c` (Shift-C remains calibration).
enum class ColorScheme {
  Default,       // semi-transparent cool panel (original look)
  Opaque,        // solid paper, no see-through
  Inverted,      // dark panel, light marks
  HighContrast,  // black panel, yellow marks
  Amber          // warm semi-transparent
};

// Per-output calibration multipliers on EDID millimetres, one per native
// panel axis (EDID width / height), so CRTC rotation keeps them correct.
struct CalFactors {
  double w = 1.0;
  double h = 1.0;
};

struct MonitorInfo {
  int x = 0, y = 0;
  int width_px = 0, height_px = 0;
  int width_mm = 0, height_mm = 0;  // EDID / XRandR panel axes (native)
  int rotation = 0;                  // RR_Rotate_* bitmask from CRTC
  std::string name;
  // Multipliers on EDID millimetres: true_mm = edid_mm * factor.
  // Set via Shift-C calibration against a real reference, per axis.
  CalFactors cal;

  bool swapped() const { return (rotation & (RR_Rotate_90 | RR_Rotate_270)) != 0; }

  // Calibration factor for the native axis lying along screen X / Y.
  double& cal_along(bool screen_y) {
    return (screen_y != swapped()) ? cal.h : cal.w;
  }
  double cal_along(bool screen_y) const {
    return (screen_y != swapped()) ? cal.h : cal.w;
  }

  // Physical millimetres along the *current* screen X / Y axes
  // (accounts for 90/270° CRTC rotation and calibration).
  double mm_along_x() const {
    const int raw = swapped() ? height_mm : width_mm;
    return std::max(1.0, static_cast<double>(raw) * cal_along(false));
  }
  double mm_along_y() const {
    const int raw = swapped() ? width_mm : height_mm;
    return std::max(1.0, static_cast<double>(raw) * cal_along(true));
  }
};

struct Options {
  double length_mm = kDefaultLengthMm;  // continuous; avoids px quantisation on resize
  int height_px = kDefaultHeightPx;
  double bg_alpha = kDefaultBgAlpha;
  Unit unit = Unit::Cm;
  ColorScheme scheme = ColorScheme::Default;
  bool vertical = false;
  // Numbers run against the screen axis (right-to-left / bottom-to-top):
  // horizontal+reversed is 180°, vertical+reversed is 270°.
  bool reversed = false;
  bool help = false;
  bool show_version = false;
  // CLI overrides config when set
  bool cli_length = false;
  bool cli_height = false;
  bool cli_alpha = false;
  bool cli_unit = false;
  bool cli_vertical = false;
  bool cli_scheme = false;
};

struct App {
  Display* dpy = nullptr;
  int screen = 0;
  Window root = 0;
  Window win = 0;
  Visual* visual = nullptr;
  Colormap colormap = 0;
  int depth = 0;
  bool has_argb = false;
  GC gc = 0;
  XRenderPictFormat* pict_format = nullptr;
  XFontStruct* font = nullptr;  // loaded once in create_window
  int rr_event_base = -1;       // XRandR event base, -1 if unavailable
  Window cal_win = 0;           // calibration instructions popup
  // Request serial of our latest move/resize; ConfigureNotify events from
  // before it are stale echoes (see handle_event).
  unsigned long last_config_serial = 0;

  Options opts;
  int win_x = 100, win_y = 100;
  int win_w = 0, win_h = 0;

  // Interaction
  // ResizeRight is the free end: right / bottom, or left / top when reversed.
  enum class DragMode { Idle, Move, ResizeRight, Origin, RotateHint, FlipHint, UnitChip };
  DragMode drag = DragMode::Idle;
  int drag_off_x = 0, drag_off_y = 0;       // window-local grab offset (move)
  int drag_root_x = 0, drag_root_y = 0;     // root grab pos (origin drag)
  int drag_origin_root = 0;                 // locked window origin in root space (resize)
  double drag_start_length = 0;
  int drag_start_origin = 0;
  int origin_mm = 0;          // numbering origin shift (mm)

  std::vector<MonitorInfo> monitors;
  const MonitorInfo* current_monitor = nullptr;
  std::string current_monitor_name;

  // Per-output calibration factors (true_mm = edid_mm * factor), keyed by
  // XRandR output name. Applied whenever monitors are (re)queried.
  std::map<std::string, CalFactors> cal_factors;

  // Shift-C calibration mode: user resizes the window to match a real 30 cm
  // object, then Enter stores the factor for the current monitor's axis
  // along the ruler (horizontal → screen X, vertical → screen Y).
  bool calibrating = false;
  bool focused = false;  // has keyboard focus: drawn as an accent frame
  int cal_target_index = kCalDefaultIndex;
  double cal_saved_length_mm = 0;
  double cal_saved_factor = 1.0;
  std::string cal_monitor_name;

  Atom atom_wm_delete = 0;
  Atom atom_net_wm_state = 0;
  Atom atom_net_wm_state_above = 0;
  Atom atom_motif_hints = 0;
};

void die(const char* msg) {
  std::fprintf(stderr, "rulertoo: %s\n", msg);
  std::exit(1);
}

// On 32-bit ARGB visuals BlackPixel/WhitePixel are often fully transparent
// (alpha=0), so core X text vanishes on light desktops. Build opaque pixels
// from the visual's channel masks instead.
unsigned long opaque_pixel(App& app, unsigned r8, unsigned g8, unsigned b8) {
  if (!app.has_argb || !app.visual)
    return (r8 << 16) | (g8 << 8) | b8;  // common TrueColor fallback
  Visual* v = app.visual;
  auto pack = [](unsigned long mask, unsigned val8) -> unsigned long {
    if (!mask) return 0;
    unsigned long bit = mask;
    // shift val into mask bits
    int shift = 0;
    while ((bit & 1) == 0) { bit >>= 1; ++shift; }
    // use top 8 bits of the mask field
    int width = 0;
    while (bit & 1) { bit >>= 1; ++width; }
    if (width <= 0) return 0;
    unsigned long v = val8;
    if (width < 8) v >>= (8 - width);
    else if (width > 8) v <<= (width - 8);
    return (v << shift) & mask;
  };
  unsigned long p = pack(v->red_mask, r8) | pack(v->green_mask, g8) | pack(v->blue_mask, b8);
  // alpha in top byte for typical ARGB
  if (app.depth == 32)
    p |= 0xff000000UL;
  return p;
}

struct Palette {
  // Background (ARGB panel)
  unsigned bg_r, bg_g, bg_b;
  double bg_alpha;          // 1.0 = fully opaque; <1 uses XRender alpha
  // Tick / border mark colour (opaque)
  unsigned mark_r, mark_g, mark_b;
  // Label chip background + text
  unsigned paper_r, paper_g, paper_b;
  unsigned text_r, text_g, text_b;
  // Length readout chip
  unsigned chip_r, chip_g, chip_b;
  unsigned chip_text_r, chip_text_g, chip_text_b;
};

Palette palette_for(ColorScheme s, double user_alpha) {
  switch (s) {
    case ColorScheme::Opaque:
      return Palette{
        /*bg*/  232, 236, 240, 1.0,
        /*mark*/ 20, 20, 20,
        /*paper*/ 255, 248, 232,  /*text*/ 0, 0, 0,
        /*chip*/ 32, 32, 32,      /*chip_text*/ 255, 255, 255,
      };
    case ColorScheme::Inverted:
      return Palette{
        /*bg*/  24, 26, 30, std::max(0.55, user_alpha),
        /*mark*/ 230, 232, 236,
        /*paper*/ 40, 42, 48,     /*text*/ 240, 240, 240,
        /*chip*/ 220, 220, 220,   /*chip_text*/ 16, 16, 16,
      };
    case ColorScheme::HighContrast:
      return Palette{
        /*bg*/  0, 0, 0, 1.0,
        /*mark*/ 255, 220, 0,
        /*paper*/ 0, 0, 0,        /*text*/ 255, 220, 0,
        /*chip*/ 255, 220, 0,     /*chip_text*/ 0, 0, 0,
      };
    case ColorScheme::Amber:
      return Palette{
        /*bg*/  210, 150, 40, std::max(0.45, user_alpha),
        /*mark*/ 40, 20, 0,
        /*paper*/ 255, 236, 180,  /*text*/ 40, 20, 0,
        /*chip*/ 60, 30, 0,       /*chip_text*/ 255, 236, 180,
      };
    case ColorScheme::Default:
    default:
      return Palette{
        /*bg*/  0xd0, 0xd8, 0xe0, user_alpha,
        /*mark*/ 0, 0, 0,
        /*paper*/ 255, 248, 232,  /*text*/ 0, 0, 0,
        /*chip*/ 32, 32, 32,      /*chip_text*/ 255, 255, 255,
      };
  }
}

const char* scheme_name(ColorScheme s) {
  switch (s) {
    case ColorScheme::Default: return "default";
    case ColorScheme::Opaque: return "opaque";
    case ColorScheme::Inverted: return "inverted";
    case ColorScheme::HighContrast: return "highcontrast";
    case ColorScheme::Amber: return "amber";
  }
  return "default";
}

bool parse_scheme_string(const std::string& s, ColorScheme* out) {
  if (s == "default" || s == "normal") { *out = ColorScheme::Default; return true; }
  if (s == "opaque" || s == "solid") { *out = ColorScheme::Opaque; return true; }
  if (s == "inverted" || s == "invert" || s == "dark") { *out = ColorScheme::Inverted; return true; }
  if (s == "highcontrast" || s == "hc" || s == "high-contrast") {
    *out = ColorScheme::HighContrast; return true;
  }
  if (s == "amber" || s == "warm") { *out = ColorScheme::Amber; return true; }
  return false;
}

Palette current_palette(const App& app) {
  return palette_for(app.opts.scheme, app.opts.bg_alpha);
}

unsigned long color_mark(App& app) {
  Palette p = current_palette(app);
  return opaque_pixel(app, p.mark_r, p.mark_g, p.mark_b);
}
unsigned long color_paper(App& app) {
  Palette p = current_palette(app);
  return opaque_pixel(app, p.paper_r, p.paper_g, p.paper_b);
}
unsigned long color_text(App& app) {
  Palette p = current_palette(app);
  return opaque_pixel(app, p.text_r, p.text_g, p.text_b);
}
unsigned long color_chip_bg(App& app) {
  Palette p = current_palette(app);
  return opaque_pixel(app, p.chip_r, p.chip_g, p.chip_b);
}
unsigned long color_chip_text(App& app) {
  Palette p = current_palette(app);
  return opaque_pixel(app, p.chip_text_r, p.chip_text_g, p.chip_text_b);
}
unsigned long color_white(App& app) { return opaque_pixel(app, 255, 255, 255); }

const char* unit_name(Unit u) {
  switch (u) {
    case Unit::Mm: return "mm";
    case Unit::Cm: return "cm";
    case Unit::Inch: return "in";
    case Unit::Pixel: return "px";
  }
  return "?";
}

void print_usage(const char* prog) {
  std::printf(
      "Usage: %s [options]\n"
      "\n"
      "Borderless on-screen ruler using real physical dimensions from XRandR.\n"
      "\n"
      "Options:\n"
      "  -l, --length MM     Physical length in millimetres (default: %d)\n"
      "  -H, --height PX     Thickness in pixels (default: %d)\n"
      "  -a, --alpha A       Background alpha 0.0-1.0 (default: %.2f)\n"
      "  -u, --unit UNIT     Display unit: cm|mm|in|px (default: cm)\n"
      "  -v, --vertical      Start in vertical orientation (same as --rotation 90)\n"
      "      --rotation DEG  Start rotated 0, 90, 180 or 270 degrees\n"
      "  -V, --version       Print version and exit\n"
      "  -h, --help          Show this help\n"
      "\n"
      "Controls:\n"
      "  Left-drag           Move the ruler\n"
      "  Drag free end       Resize length (right/bottom handle)\n"
      "  Middle-drag         Shift numbering origin\n"
      "  Right-click handle  Rotate 90 degrees clockwise (or R button / key r)\n"
      "  u                   Cycle units (cm -> mm -> in -> px)\n"
      "  c                   Cycle colour scheme (default/opaque/inverted/hc/amber)\n"
      "  r                   Rotate 90 degrees clockwise around the pointer\n"
      "  f                   Flip direction (or F button)\n"
      "  Shift-C             Start calibration (30 cm; Ctrl-C cycles targets)\n"
      "  Esc / q             Quit\n"
      "\n"
      "Position, orientation, unit, length, thickness, alpha and per-monitor\n"
      "calibration are saved to $XDG_CONFIG_HOME/rulertoo/config\n"
      "(or ~/.config/rulertoo/config) on exit. Command-line options override\n"
      "the matching saved values for that run.\n"
      "\n"
      "Calibration (Shift-C): resize until the ruler matches a real reference\n"
      "length, then Enter to store the scale for the current monitor and axis\n"
      "(horizontal or vertical; rotate and repeat for the other one) (Esc\n"
      "cancels). While calibrating, Ctrl-C cycles targets: 5/10/20/30 cm and\n"
      "2/4/8/12 in (useful on small panels that cannot fit 30 cm).\n"
      "\n",
      prog, kDefaultLengthMm, kDefaultHeightPx, kDefaultBgAlpha);
}

int orientation_deg(bool vertical, bool reversed) {
  return vertical ? (reversed ? 270 : 90) : (reversed ? 180 : 0);
}

bool parse_rotation(const std::string& s, bool* vertical, bool* reversed) {
  const int deg = std::atoi(s.c_str());
  if (s.empty() || deg % 90 != 0 || deg < 0 || deg > 270) return false;
  *vertical = (deg == 90 || deg == 270);
  *reversed = (deg >= 180);
  return true;
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "rulertoo: %s requires an argument\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
      o.help = true;
    } else if (!std::strcmp(a, "-V") || !std::strcmp(a, "--version")) {
      o.show_version = true;
    } else if (!std::strcmp(a, "-l") || !std::strcmp(a, "--length")) {
      o.length_mm = std::atof(need("--length"));
      if (o.length_mm < 20) o.length_mm = 20;
      if (o.length_mm > 2000) o.length_mm = 2000;
      o.cli_length = true;
    } else if (!std::strcmp(a, "-H") || !std::strcmp(a, "--height")) {
      o.height_px = std::atoi(need("--height"));
      if (o.height_px < 28) o.height_px = 28;
      if (o.height_px > 200) o.height_px = 200;
      o.cli_height = true;
    } else if (!std::strcmp(a, "-a") || !std::strcmp(a, "--alpha")) {
      o.bg_alpha = std::atof(need("--alpha"));
      if (o.bg_alpha < 0.0) o.bg_alpha = 0.0;
      if (o.bg_alpha > 1.0) o.bg_alpha = 1.0;
      o.cli_alpha = true;
    } else if (!std::strcmp(a, "-u") || !std::strcmp(a, "--unit")) {
      const char* u = need("--unit");
      if (!std::strcmp(u, "cm")) o.unit = Unit::Cm;
      else if (!std::strcmp(u, "mm")) o.unit = Unit::Mm;
      else if (!std::strcmp(u, "in") || !std::strcmp(u, "inch")) o.unit = Unit::Inch;
      else if (!std::strcmp(u, "px") || !std::strcmp(u, "pixel")) o.unit = Unit::Pixel;
      else {
        std::fprintf(stderr, "rulertoo: unknown unit '%s'\n", u);
        std::exit(2);
      }
      o.cli_unit = true;
    } else if (!std::strcmp(a, "-v") || !std::strcmp(a, "--vertical")) {
      o.vertical = true;
      o.reversed = false;
      o.cli_vertical = true;
    } else if (!std::strcmp(a, "--rotation")) {
      const char* d = need("--rotation");
      if (!parse_rotation(d, &o.vertical, &o.reversed)) {
        std::fprintf(stderr, "rulertoo: rotation must be 0, 90, 180 or 270\n");
        std::exit(2);
      }
      o.cli_vertical = true;
    } else {
      std::fprintf(stderr, "rulertoo: unknown option: %s\n", a);
      print_usage(argv[0]);
      std::exit(2);
    }
  }
  return o;
}

// --- Persistent config ($XDG_CONFIG_HOME/rulertoo/config) -----------------

std::string config_dir() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0])
    return std::string(xdg) + "/rulertoo";
  if (const char* home = std::getenv("HOME"); home && home[0])
    return std::string(home) + "/.config/rulertoo";
  return {};
}

std::string config_file_path() {
  std::string dir = config_dir();
  if (dir.empty()) return {};
  return dir + "/config";
}

bool ensure_config_dir() {
  std::string dir = config_dir();
  if (dir.empty()) return false;
  // Create missing parents too (e.g. ~/.config on a fresh account), or
  // settings and calibration would silently never be saved.
  for (size_t pos = dir.find('/', 1); ; pos = dir.find('/', pos + 1)) {
    const std::string part = dir.substr(0, pos);
    if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST)
      return false;
    if (pos == std::string::npos) break;
  }
  return true;
}

// Parse a single "key=value" line; returns false if not a usable pair.
bool split_kv(const char* line, std::string* key, std::string* value) {
  while (*line == ' ' || *line == '\t') ++line;
  if (*line == '\0' || *line == '#' || *line == '\n') return false;
  const char* eq = std::strchr(line, '=');
  if (!eq) return false;
  std::string k(line, eq - line);
  while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
  const char* v = eq + 1;
  while (*v == ' ' || *v == '\t') ++v;
  std::string val(v);
  while (!val.empty() && (val.back() == '\n' || val.back() == '\r' ||
                          val.back() == ' ' || val.back() == '\t'))
    val.pop_back();
  if (k.empty()) return false;
  *key = k;
  *value = val;
  return true;
}

bool parse_unit_string(const std::string& s, Unit* out) {
  if (s == "cm") { *out = Unit::Cm; return true; }
  if (s == "mm") { *out = Unit::Mm; return true; }
  if (s == "in" || s == "inch") { *out = Unit::Inch; return true; }
  if (s == "px" || s == "pixel") { *out = Unit::Pixel; return true; }
  return false;
}

// Load saved settings. CLI-set fields are left alone.
// Returns true if a position was restored into *out_x / *out_y.
// Calibration entries look like: cal.DP-1=0.982,1.013 (panel width,height).
// A single value (older configs) applies to both axes.
bool load_config(Options* opts, int* out_x, int* out_y,
                 std::map<std::string, CalFactors>* cal_out) {
  std::string path = config_file_path();
  if (path.empty()) return false;
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;

  bool have_pos = false;
  int x = 0, y = 0;
  char buf[512];
  while (std::fgets(buf, sizeof(buf), f)) {
    std::string key, val;
    if (!split_kv(buf, &key, &val)) continue;

    if (key == "x") {
      x = std::atoi(val.c_str());
      have_pos = true;
    } else if (key == "y") {
      y = std::atoi(val.c_str());
      have_pos = true;
    } else if (key == "length_mm" && !opts->cli_length) {
      double v = std::atof(val.c_str());
      if (v >= 20.0 && v <= 2000.0) opts->length_mm = v;
    } else if (key == "height_px" && !opts->cli_height) {
      int v = std::atoi(val.c_str());
      if (v >= 28 && v <= 200) opts->height_px = v;
    } else if (key == "alpha" && !opts->cli_alpha) {
      double v = std::atof(val.c_str());
      if (v >= 0.0 && v <= 1.0) opts->bg_alpha = v;
    } else if (key == "unit" && !opts->cli_unit) {
      Unit u;
      if (parse_unit_string(val, &u)) opts->unit = u;
    } else if (key == "vertical" && !opts->cli_vertical) {
      // Older configs; superseded by rotation=.
      opts->vertical = (val == "1" || val == "true" || val == "yes");
    } else if (key == "rotation" && !opts->cli_vertical) {
      parse_rotation(val, &opts->vertical, &opts->reversed);
    } else if ((key == "color" || key == "scheme") && !opts->cli_scheme) {
      ColorScheme s;
      if (parse_scheme_string(val, &s)) opts->scheme = s;
    } else if (cal_out && key.size() > 4 && key.compare(0, 4, "cal.") == 0) {
      std::string mon = key.substr(4);
      CalFactors cf;
      const size_t comma = val.find(',');
      cf.w = std::atof(val.c_str());
      cf.h = comma == std::string::npos ? cf.w : std::atof(val.c_str() + comma + 1);
      auto ok = [](double v) { return v > 0.05 && v < 20.0; };
      if (!mon.empty() && ok(cf.w) && ok(cf.h))
        (*cal_out)[mon] = cf;
    }
  }
  std::fclose(f);
  if (have_pos) {
    *out_x = x;
    *out_y = y;
  }
  return have_pos;
}

void save_config(const App& app) {
  if (!ensure_config_dir()) return;
  std::string path = config_file_path();
  if (path.empty()) return;
  // Write via temp + rename so a crash mid-write does not truncate the file.
  std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  std::fprintf(f,
               "# rulertoo config — rewritten on exit\n"
               "version=1\n"
               "x=%d\n"
               "y=%d\n"
               "length_mm=%.3f\n"
               "height_px=%d\n"
               "alpha=%.3f\n"
               "unit=%s\n"
               "rotation=%d\n"
               "color=%s\n",
               app.win_x, app.win_y,
               app.opts.length_mm, app.opts.height_px, app.opts.bg_alpha,
               unit_name(app.opts.unit), orientation_deg(app.opts.vertical, app.opts.reversed),
               scheme_name(app.opts.scheme));
  // One calibration entry (panel width,height factors) per XRandR output name
  for (const auto& kv : app.cal_factors) {
    const CalFactors& cf = kv.second;
    auto ok = [](double v) { return v > 0.05 && v < 20.0; };
    if (ok(cf.w) && ok(cf.h) &&
        (std::fabs(cf.w - 1.0) > 1e-6 || std::fabs(cf.h - 1.0) > 1e-6))
      std::fprintf(f, "cal.%s=%.6f,%.6f\n", kv.first.c_str(), cf.w, cf.h);
  }
  std::fclose(f);
  if (std::rename(tmp.c_str(), path.c_str()) != 0)
    std::remove(tmp.c_str());
}

std::vector<MonitorInfo> query_monitors(Display* dpy, Window root,
                                        const std::map<std::string, CalFactors>& cal) {
  std::vector<MonitorInfo> out;
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
  if (!res) return out;

  for (int i = 0; i < res->noutput; ++i) {
    XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
    if (!oi || oi->connection != RR_Connected || oi->crtc == 0) {
      if (oi) XRRFreeOutputInfo(oi);
      continue;
    }
    XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
    if (!ci) {
      XRRFreeOutputInfo(oi);
      continue;
    }
    MonitorInfo m;
    m.x = ci->x;
    m.y = ci->y;
    m.width_px = static_cast<int>(ci->width);
    m.height_px = static_cast<int>(ci->height);
    m.width_mm = oi->mm_width > 0 ? static_cast<int>(oi->mm_width) : 0;
    m.height_mm = oi->mm_height > 0 ? static_cast<int>(oi->mm_height) : 0;
    m.rotation = static_cast<int>(ci->rotation);
    m.name = oi->name ? oi->name : "unknown";
    auto it = cal.find(m.name);
    if (it != cal.end()) m.cal = it->second;
    if (m.width_mm > 0 && m.height_mm > 0 && m.width_px > 0)
      out.push_back(m);
    XRRFreeCrtcInfo(ci);
    XRRFreeOutputInfo(oi);
  }
  XRRFreeScreenResources(res);

  if (out.empty()) {
    MonitorInfo m;
    m.x = 0; m.y = 0;
    m.width_px = DisplayWidth(dpy, DefaultScreen(dpy));
    m.height_px = DisplayHeight(dpy, DefaultScreen(dpy));
    m.width_mm = DisplayWidthMM(dpy, DefaultScreen(dpy));
    m.height_mm = DisplayHeightMM(dpy, DefaultScreen(dpy));
    m.name = "fallback";
    auto it = cal.find(m.name);
    if (it != cal.end()) m.cal = it->second;
    out.push_back(m);
  }
  return out;
}

const MonitorInfo* find_monitor_for_point(const std::vector<MonitorInfo>& mons,
                                          int px, int py) {
  for (const auto& m : mons) {
    if (px >= m.x && px < m.x + m.width_px &&
        py >= m.y && py < m.y + m.height_px)
      return &m;
  }
  if (mons.empty()) return nullptr;
  const MonitorInfo* best = &mons[0];
  long best_d = 1L << 30;
  for (const auto& m : mons) {
    int cx = m.x + m.width_px / 2;
    int cy = m.y + m.height_px / 2;
    long dx = px - cx, dy = py - cy;
    long d = dx * dx + dy * dy;
    if (d < best_d) { best_d = d; best = &m; }
  }
  return best;
}

// Axis length in pixels / calibrated mm for the ruler's long direction.
void axis_metrics(const MonitorInfo& m, bool vertical, double* out_px, double* out_mm) {
  if (vertical) {
    *out_px = static_cast<double>(m.height_px);
    *out_mm = m.mm_along_y();
  } else {
    *out_px = static_cast<double>(m.width_px);
    *out_mm = m.mm_along_x();
  }
  if (*out_px < 1.0) *out_px = 1.0;
  if (*out_mm < 1.0) *out_mm = 1.0;
}

double px_per_mm(const MonitorInfo& m, bool vertical) {
  double px = 0, mm = 0;
  axis_metrics(m, vertical, &px, &mm);
  return px / mm;
}

// Pixel offset for a millimetre mark (continuous length supported).
int mm_to_px(const MonitorInfo& m, bool vertical, double mm) {
  double px = 0, axis_mm = 0;
  axis_metrics(m, vertical, &px, &axis_mm);
  if (mm <= 0.0) return 0;
  return static_cast<int>(std::lround(mm * px / axis_mm));
}

// Window length in pixels so the mark at length_mm falls on the last pixel index.
int length_to_win_px(const MonitorInfo& m, bool vertical, double length_mm) {
  int p = mm_to_px(m, vertical, length_mm);
  // Include the pixel of the final tick (indices 0..p inclusive → width p+1)
  return std::max(p + 1, 60);
}


void set_borderless(App& app) {
  struct {
    unsigned long flags, functions, decorations;
    long input_mode;
    unsigned long status;
  } hints = {};
  hints.flags = 2;          // MWM_HINTS_DECORATIONS
  hints.decorations = 0;
  if (app.atom_motif_hints) {
    XChangeProperty(app.dpy, app.win, app.atom_motif_hints, app.atom_motif_hints,
                    32, PropModeReplace, reinterpret_cast<unsigned char*>(&hints), 5);
  }
  // Stack above other clients and stay out of taskbar/pager when a WM does
  // still look at these properties (harmless with override_redirect).
  Atom state_atoms[3];
  int nstate = 0;
  if (app.atom_net_wm_state_above)
    state_atoms[nstate++] = app.atom_net_wm_state_above;
  Atom skip_taskbar = XInternAtom(app.dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
  Atom skip_pager = XInternAtom(app.dpy, "_NET_WM_STATE_SKIP_PAGER", False);
  if (skip_taskbar) state_atoms[nstate++] = skip_taskbar;
  if (skip_pager) state_atoms[nstate++] = skip_pager;
  if (app.atom_net_wm_state && nstate > 0) {
    XChangeProperty(app.dpy, app.win, app.atom_net_wm_state, XA_ATOM, 32,
                    PropModeReplace,
                    reinterpret_cast<unsigned char*>(state_atoms), nstate);
  }
}

// Keep the window fully visible on the current monitor when possible.
// If the ruler is longer/taller than the monitor, pin to the top-left margin
// so the origin edge stays on-screen (free end may still overhang).
void clamp_position_to_monitor(App& app) {
  if (!app.current_monitor) return;
  const MonitorInfo& m = *app.current_monitor;
  constexpr int margin = 8;
  int min_x = m.x + margin;
  int min_y = m.y + margin;
  int max_x = m.x + m.width_px - app.win_w - margin;
  int max_y = m.y + m.height_px - app.win_h - margin;
  if (max_x < min_x) max_x = min_x;
  if (max_y < min_y) max_y = min_y;
  if (app.win_x < min_x) app.win_x = min_x;
  if (app.win_y < min_y) app.win_y = min_y;
  if (app.win_x > max_x) app.win_x = max_x;
  if (app.win_y > max_y) app.win_y = max_y;
}

// Recompute win_w / win_h from length and orientation (no X request).
// Returns true if the size changed.
bool update_size(App& app) {
  if (!app.current_monitor) return false;
  int long_px = length_to_win_px(*app.current_monitor, app.opts.vertical, app.opts.length_mm);
  if (long_px < 60) long_px = 60;

  int new_w, new_h;
  if (app.opts.vertical) {
    new_w = app.opts.height_px;
    new_h = long_px;
  } else {
    new_w = long_px;
    new_h = app.opts.height_px;
  }
  if (new_w == app.win_w && new_h == app.win_h)
    return false;
  // Keep the origin edge fixed: on a reversed ruler that is the right /
  // bottom edge, so it grows to the left / upward.
  if (app.opts.reversed) {
    if (app.opts.vertical) app.win_y += app.win_h - new_h;
    else app.win_x += app.win_w - new_w;
  }
  app.win_w = new_w;
  app.win_h = new_h;
  return true;
}

// Push win_x / win_y / win_w / win_h to the server in one request, so no
// intermediate geometry (new size at the old position) is ever visible.
void commit_geometry(App& app) {
  app.last_config_serial = NextRequest(app.dpy);
  XWindowChanges ch = {};
  ch.x = app.win_x;
  ch.y = app.win_y;
  ch.width = app.win_w;
  ch.height = app.win_h;
  XConfigureWindow(app.dpy, app.win, CWX | CWY | CWWidth | CWHeight, &ch);
}

void apply_geometry(App& app) {
  // Avoid spurious ConfigureNotify / visual jumps when already at size.
  // The origin edge stays pinned so it never jumps.
  if (update_size(app))
    commit_geometry(app);
}

// Resize for a new length and pull the result fully onto the monitor.
void apply_geometry_clamped(App& app) {
  update_size(app);
  clamp_position_to_monitor(app);
  commit_geometry(app);
}

void place_on_pointer(App& app) {
  Window root_ret, child;
  int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
  unsigned mask = 0;
  if (!XQueryPointer(app.dpy, app.root, &root_ret, &child,
                     &root_x, &root_y, &win_x, &win_y, &mask)) {
    root_x = 100;
    root_y = 100;
  }
  app.monitors = query_monitors(app.dpy, app.root, app.cal_factors);
  const MonitorInfo* m = find_monitor_for_point(app.monitors, root_x, root_y);
  if (m) {
    app.current_monitor = m;
    app.current_monitor_name = m->name;
  }
  update_size(app);
  // Centre on the pointer along the long axis so vertical mode is not
  // biased toward the bottom of the screen; then clamp fully on-monitor.
  app.win_x = root_x - app.win_w / 2;
  app.win_y = root_y - app.win_h / 2;
  clamp_position_to_monitor(app);
  commit_geometry(app);
}

void create_window(App& app) {
  app.screen = DefaultScreen(app.dpy);
  app.root = RootWindow(app.dpy, app.screen);

  XVisualInfo vinfo;
  if (XMatchVisualInfo(app.dpy, app.screen, 32, TrueColor, &vinfo)) {
    app.visual = vinfo.visual;
    app.depth = vinfo.depth;
    app.colormap = XCreateColormap(app.dpy, app.root, app.visual, AllocNone);
    app.has_argb = true;
  } else {
    app.visual = DefaultVisual(app.dpy, app.screen);
    app.depth = DefaultDepth(app.dpy, app.screen);
    app.colormap = DefaultColormap(app.dpy, app.screen);
    app.has_argb = false;
  }

  XSetWindowAttributes swa = {};
  swa.colormap = app.colormap;
  swa.border_pixel = 0;
  swa.background_pixel = 0;
  // override_redirect: do not let the WM (e.g. xfwm panel struts) constrain
  // move/resize. The ruler is an overlay tool, not a normal managed window.
  swa.override_redirect = True;
  swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | StructureNotifyMask | KeyPressMask |
                   FocusChangeMask;
  unsigned long valuemask = CWColormap | CWBorderPixel | CWBackPixel |
                            CWEventMask | CWOverrideRedirect;

  app.monitors = query_monitors(app.dpy, app.root, app.cal_factors);
  app.current_monitor = app.monitors.empty() ? nullptr : &app.monitors[0];
  if (app.current_monitor)
    app.current_monitor_name = app.current_monitor->name;

  int long_px = 60;
  if (app.current_monitor)
    long_px = length_to_win_px(*app.current_monitor, app.opts.vertical, app.opts.length_mm);
  if (long_px < 60) long_px = 60;
  if (app.opts.vertical) {
    app.win_w = app.opts.height_px;
    app.win_h = long_px;
  } else {
    app.win_w = long_px;
    app.win_h = app.opts.height_px;
  }

  app.win = XCreateWindow(app.dpy, app.root, app.win_x, app.win_y, app.win_w,
                          app.win_h, 0, app.depth, InputOutput, app.visual,
                          valuemask, &swa);

  app.atom_wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(app.dpy, app.win, &app.atom_wm_delete, 1);
  app.atom_net_wm_state = XInternAtom(app.dpy, "_NET_WM_STATE", False);
  app.atom_net_wm_state_above = XInternAtom(app.dpy, "_NET_WM_STATE_ABOVE", False);
  app.atom_motif_hints = XInternAtom(app.dpy, "_MOTIF_WM_HINTS", False);

  XStoreName(app.dpy, app.win, "rulertoo");
  {
    XClassHint* ch = XAllocClassHint();
    if (ch) {
      ch->res_name = const_cast<char*>("rulertoo");
      ch->res_class = const_cast<char*>("rulertoo");
      XSetClassHint(app.dpy, app.win, ch);
      XFree(ch);
    }
  }
  set_borderless(app);

  Atom type = XInternAtom(app.dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom utility = XInternAtom(app.dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(app.dpy, app.win, type, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&utility), 1);

  app.gc = XCreateGC(app.dpy, app.win, 0, nullptr);
  app.font = XLoadQueryFont(app.dpy, "fixed");
  if (!app.font) app.font = XLoadQueryFont(app.dpy, "6x13");
  if (app.has_argb)
    app.pict_format = XRenderFindVisualFormat(app.dpy, app.visual);

  // Keep origin edge fixed on resize (do not let WM centre-gravity us).
  XSizeHints* hints = XAllocSizeHints();
  if (hints) {
    hints->flags = PWinGravity | PMinSize;
    hints->win_gravity = NorthWestGravity;
    hints->min_width = 40;
    hints->min_height = 28;
    XSetWMNormalHints(app.dpy, app.win, hints);
    XFree(hints);
  }

  XMapWindow(app.dpy, app.win);
  XRaiseWindow(app.dpy, app.win);
  XFlush(app.dpy);
}

bool refresh_monitor(App& app, bool force) {
  static int last_cx = -99999, last_cy = -99999;
  int cx = app.win_x + app.win_w / 2;
  int cy = app.win_y + app.win_h / 2;
  if (!force && std::abs(cx - last_cx) < 50 && std::abs(cy - last_cy) < 50)
    return false;
  last_cx = cx;
  last_cy = cy;

  app.monitors = query_monitors(app.dpy, app.root, app.cal_factors);
  const MonitorInfo* m = find_monitor_for_point(app.monitors, cx, cy);
  if (!m) return false;

  bool changed = (m->name != app.current_monitor_name);
  app.current_monitor = m;
  app.current_monitor_name = m->name;
  // Only re-derive pixel size when the monitor (and thus ppm) changes.
  // Forcing a resize on every ButtonRelease was causing the ruler to jump
  // after letting go of the free-end handle.
  if (changed)
    apply_geometry(app);
  return changed;
}

// True for local positions along the long axis that lie on the scale
// monitor (current_monitor). Segments that hang onto another output — or
// into the gap between monitors — are not physically valid for this scale.
std::vector<bool> scale_validity(const App& app, int long_side) {
  std::vector<bool> ok(static_cast<size_t>(std::max(long_side, 0)), false);
  if (long_side <= 0) return ok;
  if (!app.current_monitor) {
    std::fill(ok.begin(), ok.end(), true);
    return ok;
  }
  const MonitorInfo& sm = *app.current_monitor;
  const bool vert = app.opts.vertical;
  for (int i = 0; i < long_side; ++i) {
    const int rx = vert ? (app.win_x + app.win_w / 2) : (app.win_x + i);
    const int ry = vert ? (app.win_y + i) : (app.win_y + app.win_h / 2);
    if (rx >= sm.x && rx < sm.x + sm.width_px &&
        ry >= sm.y && ry < sm.y + sm.height_px)
      ok[static_cast<size_t>(i)] = true;
  }
  return ok;
}

// Invoke fn(start, end) for each contiguous true run in [0, n).
template <typename Fn>
void for_each_true_run(const std::vector<bool>& ok, Fn fn) {
  const int n = static_cast<int>(ok.size());
  int i = 0;
  while (i < n) {
    while (i < n && !ok[static_cast<size_t>(i)]) ++i;
    const int start = i;
    while (i < n && ok[static_cast<size_t>(i)]) ++i;
    if (start < i) fn(start, i);
  }
}

void mark_rect(App& app, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  Palette pal = current_palette(app);
  if (app.has_argb && app.pict_format) {
    XRenderColor c = {};
    c.red = static_cast<unsigned short>(pal.mark_r * 0x101);
    c.green = static_cast<unsigned short>(pal.mark_g * 0x101);
    c.blue = static_cast<unsigned short>(pal.mark_b * 0x101);
    c.alpha = 0xffff;
    Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
    XRenderFillRectangle(app.dpy, PictOpSrc, pict, &c, x, y, w, h);
    XRenderFreePicture(app.dpy, pict);
  } else {
    XSetForeground(app.dpy, app.gc, color_mark(app));
    XFillRectangle(app.dpy, app.win, app.gc, x, y, w, h);
  }
}

void opaque_line(App& app, int x1, int y1, int x2, int y2) {
  if (x1 == x2)
    mark_rect(app, x1, std::min(y1, y2), 1, std::abs(y2 - y1) + 1);
  else if (y1 == y2)
    mark_rect(app, std::min(x1, x2), y1, std::abs(x2 - x1) + 1, 1);
  else {
    XSetForeground(app.dpy, app.gc, color_mark(app));
    XDrawLine(app.dpy, app.win, app.gc, x1, y1, x2, y2);
  }
}

// Convert mm position to display label value based on unit + origin
double label_value(App& app, int mm_from_origin) {
  int abs_mm = mm_from_origin;  // already relative to origin
  switch (app.opts.unit) {
    case Unit::Mm: return abs_mm;
    case Unit::Cm: return abs_mm / 10.0;
    case Unit::Inch: return abs_mm / 25.4;
    case Unit::Pixel: {
      if (!app.current_monitor) return abs_mm;
      double ppm = px_per_mm(*app.current_monitor, app.opts.vertical);
      return abs_mm * ppm;
    }
  }
  return abs_mm;
}

int major_step_mm(App& app) {
  switch (app.opts.unit) {
    case Unit::Mm: return 10;      // number every 10 mm
    case Unit::Cm: return 10;      // every cm
    case Unit::Inch: return 25;    // ~ every inch (25.4 rounded)
    case Unit::Pixel: return 50;   // every 50 mm worth of pixels
  }
  return 10;
}


std::string format_length(const App& app) {
  const double mm = app.opts.length_mm;
  const char* u = unit_name(app.opts.unit);
  char buf[48];
  switch (app.opts.unit) {
    case Unit::Mm:
      std::snprintf(buf, sizeof(buf), "%.0f %s", mm, u);
      break;
    case Unit::Cm:
      std::snprintf(buf, sizeof(buf), "%.1f %s", mm / 10.0, u);
      break;
    case Unit::Inch:
      std::snprintf(buf, sizeof(buf), "%.2f %s", mm / 25.4, u);
      break;
    case Unit::Pixel: {
      double ppm = app.current_monitor
                       ? px_per_mm(*app.current_monitor, app.opts.vertical)
                       : (96.0 / 25.4);
      std::snprintf(buf, sizeof(buf), "%.0f %s", mm * ppm, u);
      break;
    }
  }
  return buf;
}

// The ruler's own frame: `a` runs along it from the origin (zero) edge to
// the free end, `c` across it. Reversed rulers mirror `a` only, so text
// stays upright while numbers run right-to-left / bottom-to-top.
struct RulerFrame {
  bool vert, rev;
  int long_side, thick;
  explicit RulerFrame(const App& app)
      : vert(app.opts.vertical), rev(app.opts.reversed),
        long_side(app.opts.vertical ? app.win_h : app.win_w),
        thick(app.opts.vertical ? app.win_w : app.win_h) {}
  // Window position along the long axis for ruler coordinate a (and back).
  int win_along(int a) const { return rev ? long_side - 1 - a : a; }
  int along(int x, int y) const { return win_along(vert ? y : x); }
  // Window-local rectangle for the ruler span [a, a+al) x [c, c+cl).
  void rect(int a, int al, int c, int cl, int* x, int* y, int* w, int* h) const {
    const int wa = rev ? long_side - a - al : a;
    if (vert) { *x = c; *y = wa; *w = cl; *h = al; }
    else { *x = wa; *y = c; *w = al; *h = cl; }
  }
};

// R (rotate) and F (flip) buttons, side by side in the middle of the ruler.
// R sits exactly in the centre: a click on it rotates around its centre, and
// a centred button maps onto itself in every orientation, so it lands back
// under the pointer. F follows in window coordinates, which flipping (an
// in-place change) leaves untouched.
void button_rects(const App& app, XRectangle* rot, XRectangle* flip) {
  const RulerFrame f(app);
  const int a0 = std::max(0, (f.long_side - kButtonPx) / 2);
  const int c0 = std::max(0, (f.thick - kButtonPx) / 2);
  auto place = [&](XRectangle* r, int a) {
    r->x = static_cast<short>(f.vert ? c0 : a);
    r->y = static_cast<short>(f.vert ? a : c0);
    r->width = r->height = kButtonPx;
  };
  place(rot, a0);
  place(flip, a0 + kButtonPx + kButtonGap);
}

bool rect_contains(const XRectangle& r, int x, int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

bool rects_overlap(const XRectangle& r, int x, int y, int w, int h) {
  return x < r.x + r.width && r.x < x + w && y < r.y + r.height && r.y < y + h;
}

// Geometry of the free-end length chip (window-local), next to the resize
// handle. In vertical mode the text is drawn rotated (reading
// bottom-to-top) when it does not fit across the ruler thickness. Returns
// false if no font.
struct ChipRect {
  int x = 0, y = 0, w = 0, h = 0;
  bool rotated = false;
};

bool length_chip_rect(const App& app, ChipRect* out) {
  XFontStruct* font = app.font;
  if (!font) return false;
  std::string len_s = format_length(app);
  const int ltw = XTextWidth(font, len_s.c_str(), static_cast<int>(len_s.size()));
  const int fh = font->ascent + font->descent;
  const int pad = 3;
  const RulerFrame f(app);
  const int band_h = std::max(14, fh + 4);
  const int band_y0 = (f.thick - band_h) / 2;
  ChipRect c;
  int along_len, cross_len, c0;
  if (f.vert) {
    c.rotated = ltw + 2 * pad > f.thick - 4;
    cross_len = (c.rotated ? fh : ltw) + 2 * pad;
    along_len = (c.rotated ? ltw : fh) + 2 * pad;
    c0 = std::max(1, (f.thick - cross_len) / 2);
  } else {
    along_len = ltw + 2 * pad;
    cross_len = fh + 2 * pad;
    c0 = std::max(1, band_y0 + 1 - pad);
  }
  const int a = std::max(1, f.long_side - kHandlePx - 4 - along_len);
  f.rect(a, along_len, c0, cross_len, &c.x, &c.y, &c.w, &c.h);
  *out = c;
  return true;
}

// Draw text rotated 90° counter-clockwise (reading bottom-to-top). The
// rotated box (w = font height, h = text width) has its top-left at (x, y).
// Core fonts cannot rotate, so render to a bitmap, transpose it and use it
// as a clip mask for a solid fill.
void draw_text_ccw(App& app, int x, int y, const char* s, int len, unsigned long pixel) {
  XFontStruct* font = app.font;
  if (!font || len <= 0) return;
  const int tw = XTextWidth(font, s, len);
  const int th = font->ascent + font->descent;
  if (tw <= 0 || th <= 0) return;

  Pixmap src = XCreatePixmap(app.dpy, app.win, tw, th, 1);
  Pixmap dst = XCreatePixmap(app.dpy, app.win, th, tw, 1);
  GC g1 = XCreateGC(app.dpy, src, 0, nullptr);
  XSetForeground(app.dpy, g1, 0);
  XFillRectangle(app.dpy, src, g1, 0, 0, tw, th);
  XSetForeground(app.dpy, g1, 1);
  XSetBackground(app.dpy, g1, 0);  // XYBitmap put maps 0 bits to background
  XSetFont(app.dpy, g1, font->fid);
  XDrawString(app.dpy, src, g1, 0, font->ascent, s, len);

  XImage* in = XGetImage(app.dpy, src, 0, 0, tw, th, 1, XYPixmap);
  const int bpl = (th + 7) / 8;
  char* data = static_cast<char*>(std::calloc(static_cast<size_t>(bpl) * tw, 1));
  XImage* out = data ? XCreateImage(app.dpy, app.visual, 1, XYBitmap, 0, data,
                                    th, tw, 8, bpl)
                     : nullptr;
  if (in && out) {
    for (int sy = 0; sy < th; ++sy)
      for (int sx = 0; sx < tw; ++sx)
        XPutPixel(out, sy, tw - 1 - sx, XGetPixel(in, sx, sy));
    XPutImage(app.dpy, dst, g1, out, 0, 0, 0, 0, th, tw);
    XSetClipMask(app.dpy, app.gc, dst);
    XSetClipOrigin(app.dpy, app.gc, x, y);
    XSetForeground(app.dpy, app.gc, pixel);
    XFillRectangle(app.dpy, app.win, app.gc, x, y, th, tw);
    XSetClipMask(app.dpy, app.gc, None);
  }
  if (out) XDestroyImage(out);  // frees data
  else std::free(data);
  if (in) XDestroyImage(in);
  XFreeGC(app.dpy, g1);
  XFreePixmap(app.dpy, src);
  XFreePixmap(app.dpy, dst);
}

void draw_cal_popup(App& app);

void draw_ruler(App& app) {
  if (!app.current_monitor) return;

  const RulerFrame frame(app);
  const bool vert = frame.vert;
  const int long_side = frame.long_side;
  const int thick = frame.thick;
  const std::vector<bool> valid = scale_validity(app, long_side);
  XRectangle btn[2];
  button_rects(app, &btn[0], &btn[1]);

  // Clear to fully transparent so off-monitor segments stay empty (outline only).
  if (app.has_argb && app.pict_format) {
    XRenderColor clear = {0, 0, 0, 0};
    Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
    XRenderFillRectangle(app.dpy, PictOpSrc, pict, &clear, 0, 0, app.win_w, app.win_h);
    XRenderFreePicture(app.dpy, pict);
  } else {
    // No real transparency: fill invalid with a dim stipple-like solid.
    XSetForeground(app.dpy, app.gc, opaque_pixel(app, 40, 40, 40));
    XFillRectangle(app.dpy, app.win, app.gc, 0, 0, app.win_w, app.win_h);
  }

  // Background only on the scale-monitor runs.
  {
    Palette pal = current_palette(app);
    const double a01 = std::clamp(pal.bg_alpha, 0.0, 1.0);
    for_each_true_run(valid, [&](int a, int b) {
      if (app.has_argb && app.pict_format) {
        XRenderColor color = {};
        unsigned short aa = static_cast<unsigned short>(a01 * 0xffff);
        color.alpha = aa;
        color.red   = static_cast<unsigned short>(pal.bg_r * a01 * 0xff);
        color.green = static_cast<unsigned short>(pal.bg_g * a01 * 0xff);
        color.blue  = static_cast<unsigned short>(pal.bg_b * a01 * 0xff);
        Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
        if (vert)
          XRenderFillRectangle(app.dpy, PictOpSrc, pict, &color, 0, a, thick, b - a);
        else
          XRenderFillRectangle(app.dpy, PictOpSrc, pict, &color, a, 0, b - a, thick);
        XRenderFreePicture(app.dpy, pict);
      } else {
        XSetForeground(app.dpy, app.gc, opaque_pixel(app, pal.bg_r, pal.bg_g, pal.bg_b));
        if (vert)
          XFillRectangle(app.dpy, app.win, app.gc, 0, a, thick, b - a);
        else
          XFillRectangle(app.dpy, app.win, app.gc, a, 0, b - a, thick);
      }
    });
  }

  XFontStruct* font = app.font;
  if (font) XSetFont(app.dpy, app.gc, font->fid);

  // Dual-side marks + centre label band with opaque backing for contrast.
  const int band_h = std::max(14, font ? (font->ascent + font->descent + 4) : 16);
  const int band_y0 = (thick - band_h) / 2;
  // Tick lengths stop before the label band so both sides stay clear
  const int margin = 1;
  const int max_tick = std::max(4, band_y0 - margin);
  const int full_h = max_tick;
  const int mid_h = std::max(3, max_tick * 2 / 3);
  const int short_h = std::max(2, max_tick / 3);

  // Centre band only on valid (scale-monitor) runs.
  for_each_true_run(valid, [&](int a, int b) {
    if (app.has_argb && app.pict_format) {
      Palette pal = current_palette(app);
      XRenderColor band = {};
      band.red = static_cast<unsigned short>(pal.paper_r * 0x101);
      band.green = static_cast<unsigned short>(pal.paper_g * 0x101);
      band.blue = static_cast<unsigned short>(pal.paper_b * 0x101);
      band.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      if (vert)
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &band, band_y0, a, band_h, b - a);
      else
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &band, a, band_y0, b - a, band_h);
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, color_paper(app));
      if (vert)
        XFillRectangle(app.dpy, app.win, app.gc, band_y0, a, band_h, b - a);
      else
        XFillRectangle(app.dpy, app.win, app.gc, a, band_y0, b - a, band_h);
    }
  });

  if (font) XSetFont(app.dpy, app.gc, font->fid);

  const int length_mm_i = static_cast<int>(std::lround(app.opts.length_mm));
  for (int mm = 0; mm <= length_mm_i; ++mm) {
    int pos = mm_to_px(*app.current_monitor, vert, mm);
    // Final tick sits on the last pixel of the content edge.
    if (mm == length_mm_i)
      pos = long_side - 1;
    pos = frame.win_along(pos);  // ruler → window (mirrored when reversed)
    if (pos < 0 || pos >= long_side) continue;
    if (!valid[static_cast<size_t>(pos)]) continue;

    int h = short_h;
    if (mm % 10 == 0) h = full_h;
    else if (mm % 5 == 0) h = mid_h;

    // Marks on BOTH outer edges
    if (vert) {
      // left edge ticks
      opaque_line(app, 0, pos, h, pos);
      // right edge ticks
      opaque_line(app, thick - 1 - h, pos, thick - 1, pos);
    } else {
      // top edge ticks
      opaque_line(app, pos, 0, pos, h);
      // bottom edge ticks
      opaque_line(app, pos, thick - 1 - h, pos, thick - 1);
    }

    // Numbers at major steps — always show a label at each major tick;
    // value is relative to origin_mm (do not skip when origin is off-grid).
    int step = major_step_mm(app);
    if (mm % step == 0 && font) {
      int rel = mm - app.origin_mm;
      double val = label_value(app, rel);
      // Avoid signed zero in labels ("-0", "-0.0").
      if (val > -0.0001 && val < 0.0001)
        val = 0.0;
      if (rel == 0)
        val = 0.0;
      char buf[24];
      if (app.opts.unit == Unit::Inch)
        std::snprintf(buf, sizeof(buf), "%.1f", val);
      else if (app.opts.unit == Unit::Pixel)
        std::snprintf(buf, sizeof(buf), "%.0f", val);
      else if (app.opts.unit == Unit::Cm)
        std::snprintf(buf, sizeof(buf), "%.0f", val);
      else
        std::snprintf(buf, sizeof(buf), "%d", rel);
      if (!std::strcmp(buf, "-0") || !std::strcmp(buf, "-0.0"))
        std::snprintf(buf, sizeof(buf), "0");

      int len = static_cast<int>(std::strlen(buf));
      int tw = XTextWidth(font, buf, len);
      int pad = 2;
      int tx, ty;
      if (vert) {
        // Centre the upright string in the thickness band and on the tick.
        tx = band_y0 + std::max(0, (band_h - tw) / 2);
        ty = pos + (font->ascent - font->descent) / 2;
        if (ty < font->ascent + 1) ty = font->ascent + 1;
        if (ty > long_side - font->descent - 1) ty = long_side - font->descent - 1;
      } else {
        tx = pos - tw / 2;
        if (tx < 2) tx = 2;
        if (tx + tw > long_side - 2) tx = long_side - 2 - tw;
        ty = band_y0 + font->ascent + (band_h - (font->ascent + font->descent)) / 2;
      }
      // Fully opaque chip behind every label (ARGB-safe via XRender).
      int cx = tx - pad;
      int cy = ty - font->ascent - pad;
      int cw = tw + 2 * pad;
      int ch = font->ascent + font->descent + 2 * pad;
      // The R / F buttons sit in the middle of the label band.
      if (rects_overlap(btn[0], cx, cy, cw, ch) || rects_overlap(btn[1], cx, cy, cw, ch))
        continue;
      if (app.has_argb && app.pict_format) {
        Palette pal = current_palette(app);
        XRenderColor chip = {};
        chip.red = static_cast<unsigned short>(pal.paper_r * 0x101);
        chip.green = static_cast<unsigned short>(pal.paper_g * 0x101);
        chip.blue = static_cast<unsigned short>(pal.paper_b * 0x101);
        chip.alpha = 0xffff;
        Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &chip, cx, cy, cw, ch);
        XRenderFreePicture(app.dpy, pict);
      } else {
        XSetForeground(app.dpy, app.gc, color_paper(app));
        XFillRectangle(app.dpy, app.win, app.gc, cx, cy, cw, ch);
      }
      XSetForeground(app.dpy, app.gc, color_text(app));
      XDrawString(app.dpy, app.win, app.gc, tx, ty, buf, len);
    }
  }

  // Solid fill of a ruler-frame rectangle.
  auto fill = [&](int a, int al, int c, int cl, unsigned r8, unsigned g8, unsigned b8) {
    int x, y, w, h;
    frame.rect(a, al, c, cl, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    if (app.has_argb && app.pict_format) {
      XRenderColor col = {};
      col.red = static_cast<unsigned short>(r8 * 0x101);
      col.green = static_cast<unsigned short>(g8 * 0x101);
      col.blue = static_cast<unsigned short>(b8 * 0x101);
      col.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &col, x, y, w, h);
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, opaque_pixel(app, r8, g8, b8));
      XFillRectangle(app.dpy, app.win, app.gc, x, y, w, h);
    }
  };

  // Resize handle on free end — high-contrast grip (visible on light and dark):
  // amber base so it stands out on black and white desktops, light inner
  // edge, dark outer edge, grip ridges.
  {
    const int h0 = long_side - kHandlePx;
    fill(h0, kHandlePx, 0, thick, 0xe0, 0x90, 0x20);
    fill(h0, 1, 0, thick, 0xff, 0xff, 0xff);
    fill(long_side - 1, 1, 0, thick, 0x00, 0x00, 0x00);
    for (int i = 2; i < kHandlePx - 1; i += 3)
      fill(h0 + i, 1, 2, thick - 4, 0xff, 0xd0, 0x80);
  }

  // R (rotate) and F (flip) buttons in the middle: chip fill, contrasting glyph.
  {
    Palette pal = current_palette(app);
    const char* glyph[2] = {"R", "F"};
    for (int i = 0; i < 2; ++i) {
      const XRectangle& b = btn[i];
      if (app.has_argb && app.pict_format) {
        XRenderColor c = {};
        c.red = static_cast<unsigned short>(pal.chip_r * 0x101);
        c.green = static_cast<unsigned short>(pal.chip_g * 0x101);
        c.blue = static_cast<unsigned short>(pal.chip_b * 0x101);
        c.alpha = 0xffff;
        Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &c, b.x, b.y, b.width, b.height);
        XRenderFreePicture(app.dpy, pict);
      } else {
        XSetForeground(app.dpy, app.gc, color_chip_bg(app));
        XFillRectangle(app.dpy, app.win, app.gc, b.x, b.y, b.width, b.height);
      }
      if (font) {
        const int gw = XTextWidth(font, glyph[i], 1);
        XSetForeground(app.dpy, app.gc, color_chip_text(app));
        XDrawString(app.dpy, app.win, app.gc, b.x + (b.width - gw) / 2,
                    b.y + (b.height + font->ascent - font->descent) / 2, glyph[i], 1);
      }
    }
  }

  // Unit is included in the free-end length chip (e.g. "30.0 cm"); no extra tag at origin.

  // Total length readout at the FREE END only (never at the origin).
  ChipRect chip;
  if (length_chip_rect(app, &chip)) {
    std::string len_s = format_length(app);
    const char* lbuf = len_s.c_str();
    int llen = static_cast<int>(len_s.size());
    const int pad = 3;
    if (app.has_argb && app.pict_format) {
      Palette pal = current_palette(app);
      XRenderColor cc = {};
      cc.red = static_cast<unsigned short>(pal.chip_r * 0x101);
      cc.green = static_cast<unsigned short>(pal.chip_g * 0x101);
      cc.blue = static_cast<unsigned short>(pal.chip_b * 0x101);
      cc.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &cc, chip.x, chip.y, chip.w, chip.h);
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, color_chip_bg(app));
      XFillRectangle(app.dpy, app.win, app.gc, chip.x, chip.y, chip.w, chip.h);
    }
    if (chip.rotated) {
      draw_text_ccw(app, chip.x + pad, chip.y + pad, lbuf, llen, color_chip_text(app));
    } else {
      XSetForeground(app.dpy, app.gc, color_chip_text(app));
      XDrawString(app.dpy, app.win, app.gc, chip.x + pad, chip.y + pad + font->ascent,
                  lbuf, llen);
    }
  }

  // Border. With keyboard focus an accent frame is added just inside it —
  // there are no WM decorations to show focus. The outline itself (which
  // carries the zero and final ticks) and the scale stay unchanged, so the
  // ruler is equally readable unfocused (no dimming / transparency).
  if (app.focused) {
    constexpr int fw = 2;
    const unsigned short iw = static_cast<unsigned short>(std::max(0, app.win_w - 2));
    const unsigned short ih = static_cast<unsigned short>(std::max(0, app.win_h - 2));
    const XRectangle edges[4] = {
      {1, 1, iw, fw},
      {1, static_cast<short>(app.win_h - 1 - fw), iw, fw},
      {1, 1, fw, ih},
      {static_cast<short>(app.win_w - 1 - fw), 1, fw, ih},
    };
    if (app.has_argb && app.pict_format) {
      XRenderColor accent = {};
      accent.red = 0x3030; accent.green = 0x9090; accent.blue = 0xffff; accent.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangles(app.dpy, PictOpSrc, pict, &accent, edges, 4);
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, opaque_pixel(app, 0x30, 0x90, 0xff));
      XFillRectangles(app.dpy, app.win, app.gc, const_cast<XRectangle*>(edges), 4);
    }
  }
  opaque_line(app, 0, 0, app.win_w - 1, 0);
  opaque_line(app, 0, app.win_h - 1, app.win_w - 1, app.win_h - 1);
  opaque_line(app, 0, 0, 0, app.win_h - 1);
  opaque_line(app, app.win_w - 1, 0, app.win_w - 1, app.win_h - 1);

  if (app.calibrating) draw_cal_popup(app);
  XFlush(app.dpy);
}

void cycle_unit(App& app) {
  switch (app.opts.unit) {
    case Unit::Cm: app.opts.unit = Unit::Mm; break;
    case Unit::Mm: app.opts.unit = Unit::Inch; break;
    case Unit::Inch: app.opts.unit = Unit::Pixel; break;
    case Unit::Pixel: app.opts.unit = Unit::Cm; break;
  }
  draw_ruler(app);
}

void cycle_scheme(App& app) {
  switch (app.opts.scheme) {
    case ColorScheme::Default:      app.opts.scheme = ColorScheme::Opaque; break;
    case ColorScheme::Opaque:       app.opts.scheme = ColorScheme::Inverted; break;
    case ColorScheme::Inverted:     app.opts.scheme = ColorScheme::HighContrast; break;
    case ColorScheme::HighContrast: app.opts.scheme = ColorScheme::Amber; break;
    case ColorScheme::Amber:        app.opts.scheme = ColorScheme::Default; break;
  }
  draw_ruler(app);
}

// Change orientation so the ruler point under the pivot (root coordinates)
// stays under it, then pull the ruler onto the monitor (which can move it
// off the pivot near screen edges; a cut-off ruler is worse). The across
// axis turns with the ruler like a real rotation. With `on_button` the
// pivot is the R button centre and the new window is placed so the R button
// is centred on it exactly; the generic mm round trip drifts a pixel or two
// per turn, which walked the button out from under a stationary pointer.
void set_orientation(App& app, bool vert, bool rev, int px, int py, bool on_button) {
  // The calibrated axis follows the orientation, so keep it fixed meanwhile.
  if (app.calibrating || !app.current_monitor) return;
  const MonitorInfo& m = *app.current_monitor;
  const int thick = app.opts.height_px;
  const int old_long = app.opts.vertical ? app.win_h : app.win_w;
  const int lx = std::clamp(px - app.win_x, 0, app.win_w - 1);
  const int ly = std::clamp(py - app.win_y, 0, app.win_h - 1);

  // Pivot in ruler terms: u along from the origin, v across (turning with it).
  int u = 0, v = 0;
  switch (orientation_deg(app.opts.vertical, app.opts.reversed)) {
    case 0:   u = lx;                v = ly;             break;
    case 90:  u = ly;                v = thick - 1 - lx; break;
    case 180: u = old_long - 1 - lx; v = thick - 1 - ly; break;
    default:  u = old_long - 1 - ly; v = lx;             break;
  }
  // Physical distance, since the axes can have different px/mm.
  const double u_mm = u / px_per_mm(m, app.opts.vertical);

  app.opts.vertical = vert;
  app.opts.reversed = rev;
  update_size(app);
  if (on_button) {
    XRectangle rot, flip;
    button_rects(app, &rot, &flip);
    app.win_x = px - (rot.x + rot.width / 2);
    app.win_y = py - (rot.y + rot.height / 2);
    clamp_position_to_monitor(app);
    commit_geometry(app);
    draw_ruler(app);
    return;
  }
  const int new_long = vert ? app.win_h : app.win_w;
  u = std::clamp(static_cast<int>(std::lround(u_mm * px_per_mm(m, vert))), 0, new_long - 1);
  int nx = 0, ny = 0;
  switch (orientation_deg(vert, rev)) {
    case 0:   nx = u;                ny = v;                break;
    case 90:  nx = thick - 1 - v;    ny = u;                break;
    case 180: nx = new_long - 1 - u; ny = thick - 1 - v;    break;
    default:  nx = v;                ny = new_long - 1 - u; break;
  }
  app.win_x = px - nx;
  app.win_y = py - ny;
  clamp_position_to_monitor(app);
  commit_geometry(app);
  draw_ruler(app);
}

// 0° → 90° → 180° → 270° → 0°, clockwise.
void rotate_cw(App& app, int px, int py, bool on_button) {
  const bool vert = !app.opts.vertical;
  const bool rev = app.opts.vertical ? !app.opts.reversed : app.opts.reversed;
  set_orientation(app, vert, rev, px, py, on_button);
}

// Reverse the numbering direction in place. Same axis, same size, so the
// window does not move at all; only the drawing changes. Allowed while
// calibrating since the calibrated axis is unaffected.
void flip_direction(App& app) {
  app.opts.reversed = !app.opts.reversed;
  draw_ruler(app);
}

// Pivot for keyboard rotation: the pointer when it is over the ruler,
// otherwise the ruler's centre. Over the R button it behaves like a click
// on it (returns true with the button centre as pivot).
bool keyboard_pivot(App& app, int* px, int* py) {
  *px = app.win_x + app.win_w / 2;
  *py = app.win_y + app.win_h / 2;
  Window root_ret, child;
  int rx = 0, ry = 0, wx = 0, wy = 0;
  unsigned mask = 0;
  if (XQueryPointer(app.dpy, app.root, &root_ret, &child, &rx, &ry, &wx, &wy, &mask) &&
      rx >= app.win_x && rx < app.win_x + app.win_w &&
      ry >= app.win_y && ry < app.win_y + app.win_h) {
    XRectangle rot, flip;
    button_rects(app, &rot, &flip);
    if (rect_contains(rot, rx - app.win_x, ry - app.win_y)) {
      *px = app.win_x + rot.x + rot.width / 2;
      *py = app.win_y + rot.y + rot.height / 2;
      return true;
    }
    *px = rx;
    *py = ry;
  }
  return false;
}

// Calibration instructions live in their own small window next to the
// ruler: a vertical ruler (or a short horizontal one) is far too narrow to
// hold the text.
void cal_popup_lines(const App& app, std::string lines[4]) {
  const int ti = std::clamp(app.cal_target_index, 0, kCalTargetCount - 1);
  lines[0] = "CALIBRATION";
  lines[1] = std::string("Resize to real ") + kCalTargets[ti].label;
  lines[2] = std::string(app.opts.vertical ? "Vertical" : "Horizontal") + " axis of " +
             (app.current_monitor ? app.current_monitor->name : std::string("?"));
  lines[3] = "Enter=save  Esc=cancel  Ctrl-C=next";
}

void draw_cal_popup(App& app) {
  if (!app.cal_win || !app.font) return;
  XFontStruct* font = app.font;
  std::string lines[4];
  cal_popup_lines(app, lines);
  const int pad = 6;
  const int lh = font->ascent + font->descent + 2;
  int w = 0;
  for (const auto& l : lines)
    w = std::max(w, XTextWidth(font, l.c_str(), static_cast<int>(l.size())));
  w += 2 * pad;
  const int h = 4 * lh + 2 * pad;

  // Beside the ruler: below/above a horizontal one, right/left of a
  // vertical one, whichever side has room on the monitor.
  constexpr int gap = 6;
  int x, y;
  if (app.opts.vertical) {
    x = app.win_x + app.win_w + gap;
    y = app.win_y;
  } else {
    x = app.win_x;
    y = app.win_y + app.win_h + gap;
  }
  if (app.current_monitor) {
    const MonitorInfo& m = *app.current_monitor;
    if (app.opts.vertical && x + w > m.x + m.width_px)
      x = app.win_x - gap - w;
    if (!app.opts.vertical && y + h > m.y + m.height_px)
      y = app.win_y - gap - h;
    x = std::clamp(x, m.x, std::max(m.x, m.x + m.width_px - w));
    y = std::clamp(y, m.y, std::max(m.y, m.y + m.height_px - h));
  }
  XMoveResizeWindow(app.dpy, app.cal_win, x, y, w, h);
  XRaiseWindow(app.dpy, app.cal_win);

  XSetForeground(app.dpy, app.gc, opaque_pixel(app, 192, 32, 16));
  XFillRectangle(app.dpy, app.cal_win, app.gc, 0, 0, w, h);
  XSetForeground(app.dpy, app.gc, color_white(app));
  XSetFont(app.dpy, app.gc, font->fid);
  for (int i = 0; i < 4; ++i)
    XDrawString(app.dpy, app.cal_win, app.gc, pad, pad + i * lh + font->ascent,
                lines[i].c_str(), static_cast<int>(lines[i].size()));
}

void open_cal_popup(App& app) {
  if (app.cal_win) return;
  XSetWindowAttributes swa = {};
  swa.colormap = app.colormap;
  swa.border_pixel = 0;
  swa.background_pixel = opaque_pixel(app, 192, 32, 16);
  swa.override_redirect = True;
  swa.event_mask = ExposureMask;
  app.cal_win = XCreateWindow(app.dpy, app.root, 0, 0, 1, 1, 0, app.depth,
                              InputOutput, app.visual,
                              CWColormap | CWBorderPixel | CWBackPixel |
                                  CWOverrideRedirect | CWEventMask,
                              &swa);
  XMapRaised(app.dpy, app.cal_win);
}

void close_cal_popup(App& app) {
  if (!app.cal_win) return;
  XDestroyWindow(app.dpy, app.cal_win);
  app.cal_win = 0;
}

void begin_calibration(App& app) {
  if (!app.current_monitor) return;
  if (app.calibrating) return;
  app.calibrating = true;
  app.cal_target_index = kCalDefaultIndex;  // 30 cm first
  app.cal_saved_length_mm = app.opts.length_mm;
  app.cal_saved_factor = app.current_monitor->cal_along(app.opts.vertical);
  app.cal_monitor_name = app.current_monitor->name;
  // Start near the target length so the user only fine-tunes; keep it on
  // the monitor so no part of the reference span is blanked.
  app.opts.length_mm = kCalTargets[app.cal_target_index].mm;
  open_cal_popup(app);
  apply_geometry_clamped(app);
  draw_ruler(app);
}

void cycle_cal_target(App& app) {
  if (!app.calibrating) return;
  app.cal_target_index = (app.cal_target_index + 1) % kCalTargetCount;
  app.opts.length_mm = kCalTargets[app.cal_target_index].mm;
  apply_geometry_clamped(app);
  draw_ruler(app);
}

void cancel_calibration(App& app) {
  if (!app.calibrating) return;
  app.calibrating = false;
  close_cal_popup(app);
  app.opts.length_mm = app.cal_saved_length_mm;
  // Restore factor on the live monitor entry if still the same output.
  if (app.current_monitor && app.current_monitor->name == app.cal_monitor_name) {
    // Mutate via the vector entry (current_monitor points into monitors).
    for (auto& m : app.monitors) {
      if (m.name == app.cal_monitor_name) {
        m.cal_along(app.opts.vertical) = app.cal_saved_factor;
        app.current_monitor = &m;
        break;
      }
    }
  }
  apply_geometry(app);
  draw_ruler(app);
}

void confirm_calibration(App& app) {
  if (!app.calibrating || !app.current_monitor) return;
  // Pixel span currently on screen corresponds to the active cal target.
  // EDID-based length for the same span (with factor=1) is:
  //   edid_mm = last_tick * edid_axis_mm / axis_px
  // factor = true_mm / edid_mm, stored for the native axis along the ruler.
  const double target_mm = kCalTargets[std::clamp(app.cal_target_index, 0, kCalTargetCount - 1)].mm;
  const bool vert = app.opts.vertical;
  const int span_px = vert ? app.win_h : app.win_w;
  int last_tick = span_px - 1;
  if (last_tick < 1) last_tick = 1;

  // Raw EDID millimetres (ignore current factor).
  const MonitorInfo& m = *app.current_monitor;
  const int raw_mm = (vert != m.swapped()) ? m.height_mm : m.width_mm;
  const int raw_px = vert ? m.height_px : m.width_px;
  if (raw_mm <= 0 || raw_px <= 0) {
    cancel_calibration(app);
    return;
  }
  const double edid_mm = static_cast<double>(last_tick) * static_cast<double>(raw_mm)
                         / static_cast<double>(raw_px);
  if (edid_mm < 1.0) {
    cancel_calibration(app);
    return;
  }
  double factor = target_mm / edid_mm;
  if (factor < 0.05) factor = 0.05;
  if (factor > 20.0) factor = 20.0;

  // The span was measured on the current monitor (the ruler may have been
  // moved to another output since Shift-C), so store the factor there.
  const std::string mon_name = m.name;
  for (auto& mon : app.monitors) {
    if (mon.name == mon_name) {
      mon.cal_along(vert) = factor;
      app.cal_factors[mon_name] = mon.cal;
      app.current_monitor = &mon;
      break;
    }
  }
  app.calibrating = false;
  close_cal_popup(app);
  // Keep the window size; length now correctly reports the target.
  app.opts.length_mm = target_mm;
  apply_geometry(app);
  draw_ruler(app);
  // Persist immediately so a crash does not lose the calibration.
  save_config(app);
}

void quit(App& app) {
  if (app.calibrating)
    cancel_calibration(app);
  save_config(app);
  XUnmapWindow(app.dpy, app.win);
  XDestroyWindow(app.dpy, app.win);
  XCloseDisplay(app.dpy);
  std::exit(0);
}

App::DragMode hit_test(App& app, int x, int y) {
  const RulerFrame frame(app);

  // Resize handle on the free end only
  if (frame.along(x, y) >= frame.long_side - kHandlePx)
    return App::DragMode::ResizeRight;

  XRectangle rot, flip;
  button_rects(app, &rot, &flip);
  if (rect_contains(rot, x, y)) return App::DragMode::RotateHint;
  if (rect_contains(flip, x, y)) return App::DragMode::FlipHint;

  // Length chip toggles units on click
  ChipRect c;
  if (length_chip_rect(app, &c) &&
      x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h)
    return App::DragMode::UnitChip;

  return App::DragMode::Move;
}

void handle_event(App& app, XEvent& ev) {
  switch (ev.type) {
    case Expose:
      if (ev.xexpose.count != 0) break;
      if (app.cal_win && ev.xexpose.window == app.cal_win)
        draw_cal_popup(app);
      else
        draw_ruler(app);
      break;

    case ConfigureNotify: {
      // app.win_* is authoritative for our own requests. Events generated
      // before our latest request lag behind it: copying their geometry
      // back made off-monitor blanking draw against a stale position (e.g.
      // a half-blank ruler right after rotating). Newer ones are either the
      // echo of that request or an external move (xdotool, a WM ignoring
      // override-redirect), which we follow.
      const XConfigureEvent& ce = ev.xconfigure;
      if (ce.window != app.win || ce.serial < app.last_config_serial)
        break;
      if (app.drag == App::DragMode::ResizeRight)
        break;
      if (ce.width != app.win_w || ce.height != app.win_h) {
        commit_geometry(app);  // keep the physical length
        break;
      }
      if (ce.x != app.win_x || ce.y != app.win_y) {
        app.win_x = ce.x;
        app.win_y = ce.y;
        refresh_monitor(app, true);
        draw_ruler(app);
      }
      break;
    }

    case FocusIn:
    case FocusOut: {
      // Temporary keyboard grabs (WM key bindings, alt-tab while held) do
      // not really move focus; ignore them to avoid flicker.
      if (ev.xfocus.mode == NotifyGrab || ev.xfocus.mode == NotifyUngrab)
        break;
      const bool f = (ev.type == FocusIn);
      if (f != app.focused) {
        app.focused = f;
        draw_ruler(app);
      }
      break;
    }

    case ButtonPress: {
      int bx = ev.xbutton.x;
      int by = ev.xbutton.y;
      // Ignore scroll wheel (often reported as Button4/5); it was shifting
      // origin / clearing labels when mis-handled as middle interactions.
      if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
        break;
      // Overlay window: keep above the panel and accept keys while interacting.
      XRaiseWindow(app.dpy, app.win);
      XSetInputFocus(app.dpy, app.win, RevertToParent, CurrentTime);
      if (ev.xbutton.button == Button1) {
        App::DragMode mode = hit_test(app, bx, by);
        if (mode == App::DragMode::RotateHint) {
          // Pivot on the button centre rather than the exact click so the
          // (centred) button ends up under the pointer again.
          XRectangle rot, flip;
          button_rects(app, &rot, &flip);
          rotate_cw(app, app.win_x + rot.x + rot.width / 2, app.win_y + rot.y + rot.height / 2,
                    true);
          break;
        }
        if (mode == App::DragMode::FlipHint) {
          flip_direction(app);
          break;
        }
        if (mode == App::DragMode::UnitChip) {
          cycle_unit(app);
          break;
        }
        app.drag = mode;
        app.drag_off_x = bx;
        app.drag_off_y = by;
        app.drag_root_x = ev.xbutton.x_root;
        app.drag_root_y = ev.xbutton.y_root;
        app.drag_start_length = app.opts.length_mm;
        // Lock origin edge in root space for the whole resize gesture.
        // Using live win_x/win_y during drag caused the left edge to jump
        // (ConfigureNotify feedback / WM gravity).
        // Reversed rulers have their origin at the right / bottom edge.
        if (app.opts.vertical)
          app.drag_origin_root = app.opts.reversed ? app.win_y + app.win_h : app.win_y;
        else
          app.drag_origin_root = app.opts.reversed ? app.win_x + app.win_w : app.win_x;
      } else if (ev.xbutton.button == Button2) {
        // Middle: drag origin
        app.drag = App::DragMode::Origin;
        app.drag_off_x = bx;
        app.drag_off_y = by;
        app.drag_root_x = ev.xbutton.x_root;
        app.drag_root_y = ev.xbutton.y_root;
        app.drag_start_origin = app.origin_mm;
      } else if (ev.xbutton.button == Button3) {
        // Right on handle area -> rotate, else quit
        App::DragMode mode = hit_test(app, bx, by);
        if (mode == App::DragMode::ResizeRight || mode == App::DragMode::RotateHint)
          rotate_cw(app, ev.xbutton.x_root, ev.xbutton.y_root, false);
        else
          quit(app);
      }
      break;
    }

    case ButtonRelease:
      if (ev.xbutton.button == Button1 || ev.xbutton.button == Button2) {
        const bool was_resize = (app.drag == App::DragMode::ResizeRight);
        app.drag = App::DragMode::Idle;
        // Re-query monitor only after a move; never force a size change after
        // resize. Skip the motion hysteresis so a short move across an
        // output border still picks up the right scale.
        if (!was_resize)
          refresh_monitor(app, true);
        draw_ruler(app);
      }
      break;

    case MotionNotify:
      if (app.drag == App::DragMode::Idle) break;
      // Coalesce
      while (XPending(app.dpy)) {
        XEvent peek;
        XPeekEvent(app.dpy, &peek);
        if (peek.type != MotionNotify) break;
        XNextEvent(app.dpy, &ev);
      }
      {
        int mx = ev.xmotion.x_root;
        int my = ev.xmotion.y_root;

        if (app.drag == App::DragMode::Move) {
          int nx = mx - app.drag_off_x;
          int ny = my - app.drag_off_y;
          app.win_x = nx;
          app.win_y = ny;
          app.last_config_serial = NextRequest(app.dpy);
          XMoveWindow(app.dpy, app.win, nx, ny);
          // Update scale monitor when the centre crosses an output, and always
          // redraw so off-monitor blanking tracks the live position.
          refresh_monitor(app, false);
          draw_ruler(app);
        } else if (app.drag == App::DragMode::ResizeRight) {
          if (!app.current_monitor) break;
          // Length from locked origin → pointer (root space). Origin is fixed
          // for the gesture so the left/top edge cannot jump to 0.
          int pointer = app.opts.vertical ? my : mx;
          int span_px = app.opts.reversed ? app.drag_origin_root - 1 - pointer
                                          : pointer - app.drag_origin_root;
          if (span_px < 60) span_px = 60;
          // Continuous length: window span maps 1:1 to pixels (no mm quantisation).
          double ax_px = 0, ax_mm = 0;
          axis_metrics(*app.current_monitor, app.opts.vertical, &ax_px, &ax_mm);
          int last_tick = span_px - 1;
          if (last_tick < 1) last_tick = 1;
          double new_len = static_cast<double>(last_tick) * ax_mm / ax_px;
          if (new_len < 30.0) new_len = 30.0;
          if (new_len > 2000.0) new_len = 2000.0;
          // Always apply so 1px pointer moves produce 1px window changes.
          app.opts.length_mm = new_len;
          apply_geometry(app);
          draw_ruler(app);
        } else if (app.drag == App::DragMode::Origin) {
          if (!app.current_monitor) break;
          double ppm = px_per_mm(*app.current_monitor, app.opts.vertical);
          int delta_px = app.opts.vertical
                             ? (my - app.drag_root_y)
                             : (mx - app.drag_root_x);
          if (app.opts.reversed) delta_px = -delta_px;
          int delta_mm = static_cast<int>(std::lround(delta_px / ppm));
          app.origin_mm = app.drag_start_origin + delta_mm;
          draw_ruler(app);
        }
      }
      break;

    case KeyPress: {
      KeySym ks = XLookupKeysym(&ev.xkey, 0);
      const unsigned st = ev.xkey.state;
      if (app.calibrating) {
        if (ks == XK_Escape)
          cancel_calibration(app);
        else if (ks == XK_Return || ks == XK_KP_Enter)
          confirm_calibration(app);
        else if ((ks == XK_c || ks == XK_C) && (st & ControlMask))
          cycle_cal_target(app);
        // Ignore other keys while calibrating (q must not quit mid-cal).
        break;
      }
      if (ks == XK_Escape || ks == XK_q)
        quit(app);
      else if (ks == XK_u || ks == XK_U)
        cycle_unit(app);
      else if (ks == XK_r || ks == XK_R) {
        int px, py;
        const bool on_button = keyboard_pivot(app, &px, &py);
        rotate_cw(app, px, py, on_button);
      }
      else if (ks == XK_f || ks == XK_F)
        flip_direction(app);
      else if ((ks == XK_c || ks == XK_C) && (st & ShiftMask))
        begin_calibration(app);
      else if (ks == XK_c || ks == XK_C)
        cycle_scheme(app);
      break;
    }

    case ClientMessage:
      if (static_cast<Atom>(ev.xclient.data.l[0]) == app.atom_wm_delete)
        quit(app);
      break;

    default:
      if (app.rr_event_base >= 0 && ev.type == app.rr_event_base + RRScreenChangeNotify) {
        // Monitor layout / mode / rotation changed: re-derive the scale.
        XRRUpdateConfiguration(&ev);
        refresh_monitor(app, true);
        apply_geometry(app);
        draw_ruler(app);
      }
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  App app;
  app.opts = parse_args(argc, argv);
  if (app.opts.help) {
    print_usage(argv[0]);
    return 0;
  }
  if (app.opts.show_version) {
    std::printf("rulertoo %s\n", RULERTOO_VERSION);
    return 0;
  }

  int saved_x = 0, saved_y = 0;
  bool have_saved_pos = load_config(&app.opts, &saved_x, &saved_y, &app.cal_factors);
  if (have_saved_pos) {
    app.win_x = saved_x;
    app.win_y = saved_y;
  }

  app.dpy = XOpenDisplay(nullptr);
  if (!app.dpy) die("cannot open X display");

  int event_base, error_base;
  const bool have_rr = XRRQueryExtension(app.dpy, &event_base, &error_base);
  if (!have_rr)
    std::fprintf(stderr, "rulertoo: XRandR not available, using fallback sizes\n");

  create_window(app);
  if (have_rr) {
    app.rr_event_base = event_base;
    XRRSelectInput(app.dpy, app.root, RRScreenChangeNotifyMask);
  }
  if (have_saved_pos) {
    // Restore monitor for the saved position, then size + move.
    app.monitors = query_monitors(app.dpy, app.root, app.cal_factors);
    const MonitorInfo* m =
        find_monitor_for_point(app.monitors, app.win_x + 8, app.win_y + 8);
    if (m) {
      app.current_monitor = m;
      app.current_monitor_name = m->name;
    }
    apply_geometry_clamped(app);
  } else {
    place_on_pointer(app);
  }
  draw_ruler(app);

  XEvent ev;
  while (true) {
    XNextEvent(app.dpy, &ev);
    handle_event(app, ev);
  }
  return 0;
}
