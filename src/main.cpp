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
constexpr int kRotateBtn = 18;         // size of rotate button area
constexpr double kCalTargetMm = 300.0; // real-world length used during calibration

enum class Unit { Mm, Cm, Inch, Pixel };

// Visual themes cycled with `c` (Shift-C remains calibration).
enum class ColorScheme {
  Default,       // semi-transparent cool panel (original look)
  Opaque,        // solid paper, no see-through
  Inverted,      // dark panel, light marks
  HighContrast,  // black panel, yellow marks
  Amber          // warm semi-transparent
};

struct MonitorInfo {
  int x = 0, y = 0;
  int width_px = 0, height_px = 0;
  int width_mm = 0, height_mm = 0;  // EDID / XRandR panel axes (native)
  int rotation = 0;                  // RR_Rotate_* bitmask from CRTC
  std::string name;
  // Multiplier on EDID millimetres: true_mm = edid_mm * cal_factor.
  // Set via Shift-C calibration against a real 30 cm reference.
  double cal_factor = 1.0;

  // Physical millimetres along the *current* screen X / Y axes
  // (accounts for 90/270° CRTC rotation and calibration).
  double mm_along_x() const {
    const int raw = (rotation & (RR_Rotate_90 | RR_Rotate_270)) ? height_mm : width_mm;
    return std::max(1.0, static_cast<double>(raw) * cal_factor);
  }
  double mm_along_y() const {
    const int raw = (rotation & (RR_Rotate_90 | RR_Rotate_270)) ? width_mm : height_mm;
    return std::max(1.0, static_cast<double>(raw) * cal_factor);
  }
};

struct Options {
  double length_mm = kDefaultLengthMm;  // continuous; avoids px quantisation on resize
  int height_px = kDefaultHeightPx;
  double bg_alpha = kDefaultBgAlpha;
  Unit unit = Unit::Cm;
  ColorScheme scheme = ColorScheme::Default;
  bool vertical = false;
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

  Options opts;
  int win_x = 100, win_y = 100;
  int win_w = 0, win_h = 0;

  // Interaction
  enum class DragMode { Idle, Move, ResizeRight, Origin, RotateHint, UnitChip };
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
  std::map<std::string, double> cal_factors;

  // Shift-C calibration mode: user resizes the window to match a real 30 cm
  // object, then Enter stores the factor for the current monitor.
  bool calibrating = false;
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
unsigned long color_black(App& app) { return opaque_pixel(app, 0, 0, 0); }

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
      "  -v, --vertical      Start in vertical orientation\n"
      "  -V, --version       Print version and exit\n"
      "  -h, --help          Show this help\n"
      "\n"
      "Controls:\n"
      "  Left-drag           Move the ruler\n"
      "  Drag free end         Resize length (right/bottom handle)\n"
      "  Middle-drag        Shift numbering origin\n"
      "  Right-click handle   Toggle orientation (or R button / key r)\n"
      "  u                  Cycle units (cm -> mm -> in -> px)\n"
      "  c                  Cycle colour scheme (default/opaque/inverted/hc/amber)\n"
      "  r                  Rotate horizontal/vertical\n"
      "  Shift-C            Calibrate against a real 30 cm reference\n"
      "  Esc / q            Quit\n"
      "\n"
      "Position, orientation, unit, length, thickness, alpha and per-monitor\n"
      "calibration are saved to $XDG_CONFIG_HOME/rulertoo/config\n"
      "(or ~/.config/rulertoo/config) on exit. Command-line options override\n"
      "the matching saved values for that run.\n"
      "\n"
      "Calibration (Shift-C): resize the ruler until it matches a real 30 cm\n"
      "object, then press Enter to store the scale for the current monitor\n"
      "(Esc cancels). Useful when EDID millimetre sizes are wrong.\n"
      "\n",
      prog, kDefaultLengthMm, kDefaultHeightPx, kDefaultBgAlpha);
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
  if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
    return false;
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
// Calibration entries look like: cal.DP-1=0.982
bool load_config(Options* opts, int* out_x, int* out_y,
                 std::map<std::string, double>* cal_out) {
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
      opts->vertical = (val == "1" || val == "true" || val == "yes");
    } else if ((key == "color" || key == "scheme") && !opts->cli_scheme) {
      ColorScheme s;
      if (parse_scheme_string(val, &s)) opts->scheme = s;
    } else if (cal_out && key.size() > 4 && key.compare(0, 4, "cal.") == 0) {
      std::string mon = key.substr(4);
      double fct = std::atof(val.c_str());
      if (!mon.empty() && fct > 0.05 && fct < 20.0)
        (*cal_out)[mon] = fct;
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
               "vertical=%d\n"
               "color=%s\n",
               app.win_x, app.win_y,
               app.opts.length_mm, app.opts.height_px, app.opts.bg_alpha,
               unit_name(app.opts.unit), app.opts.vertical ? 1 : 0,
               scheme_name(app.opts.scheme));
  // One calibration factor per XRandR output name
  for (const auto& kv : app.cal_factors) {
    if (kv.second > 0.05 && kv.second < 20.0 &&
        std::fabs(kv.second - 1.0) > 1e-6)
      std::fprintf(f, "cal.%s=%.6f\n", kv.first.c_str(), kv.second);
  }
  std::fclose(f);
  if (std::rename(tmp.c_str(), path.c_str()) != 0)
    std::remove(tmp.c_str());
}

std::vector<MonitorInfo> query_monitors(Display* dpy, Window root,
                                        const std::map<std::string, double>& cal) {
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
    if (it != cal.end()) m.cal_factor = it->second;
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
    if (it != cal.end()) m.cal_factor = it->second;
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

void apply_geometry(App& app) {
  if (!app.current_monitor) return;
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
  // Avoid spurious ConfigureNotify / visual jumps when already at size.
  if (new_w == app.win_w && new_h == app.win_h)
    return;
  app.win_w = new_w;
  app.win_h = new_h;
  // Pin top-left (or top for vertical growth downward) so the origin edge
  // never jumps when the WM interprets a bare resize.
  XWindowChanges ch = {};
  ch.x = app.win_x;
  ch.y = app.win_y;
  ch.width = app.win_w;
  ch.height = app.win_h;
  XConfigureWindow(app.dpy, app.win, CWX | CWY | CWWidth | CWHeight, &ch);
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
  apply_geometry(app);
  // Centre on the pointer along the long axis so vertical mode is not
  // biased toward the bottom of the screen; then clamp fully on-monitor.
  int nx = root_x - app.win_w / 2;
  int ny = root_y - app.win_h / 2;
  app.win_x = nx;
  app.win_y = ny;
  clamp_position_to_monitor(app);
  XMoveWindow(app.dpy, app.win, app.win_x, app.win_y);
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
  (void)force;
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

void fill_bg(App& app) {
  Palette pal = current_palette(app);
  if (app.has_argb && app.pict_format) {
    XRenderColor color = {};
    const double a01 = std::clamp(pal.bg_alpha, 0.0, 1.0);
    unsigned short a = static_cast<unsigned short>(a01 * 0xffff);
    color.alpha = a;
    // Premultiply for PictOpSrc with partial alpha
    color.red   = static_cast<unsigned short>(pal.bg_r * a01 * 0xff);
    color.green = static_cast<unsigned short>(pal.bg_g * a01 * 0xff);
    color.blue  = static_cast<unsigned short>(pal.bg_b * a01 * 0xff);
    Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
    XRenderFillRectangle(app.dpy, PictOpSrc, pict, &color, 0, 0, app.win_w, app.win_h);
    XRenderFreePicture(app.dpy, pict);
  } else {
    XSetForeground(app.dpy, app.gc, opaque_pixel(app, pal.bg_r, pal.bg_g, pal.bg_b));
    XFillRectangle(app.dpy, app.win, app.gc, 0, 0, app.win_w, app.win_h);
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

void opaque_rect(App& app, int x, int y, int w, int h) {
  mark_rect(app, x, y, w, h);
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

// Geometry of the free-end length chip (window-local). Returns false if no font.
bool length_chip_rect(App& app, XFontStruct* font, int* out_x, int* out_y, int* out_w, int* out_h) {
  if (!font) return false;
  std::string len_s = format_length(app);
  int ltw = XTextWidth(font, len_s.c_str(), static_cast<int>(len_s.size()));
  const int pad = 3;
  const bool vert = app.opts.vertical;
  const int long_side = vert ? app.win_h : app.win_w;
  const int thick = vert ? app.win_w : app.win_h;
  const int band_h = std::max(14, font->ascent + font->descent + 4);
  const int band_y0 = (thick - band_h) / 2;
  int lx, ly;
  if (vert) {
    lx = band_y0 + 2;
    ly = long_side - kHandlePx - kRotateBtn - 18;
  } else {
    lx = long_side - kHandlePx - kRotateBtn - ltw - 10;
    ly = band_y0 + font->ascent + 1;
  }
  if (lx < 2) lx = 2;
  if (ly < font->ascent + 2) ly = font->ascent + 2;
  *out_x = lx - pad;
  *out_y = ly - font->ascent - pad;
  *out_w = ltw + 2 * pad;
  *out_h = font->ascent + font->descent + 2 * pad;
  return true;
}


void draw_ruler(App& app) {
  if (!app.current_monitor) return;

  const bool vert = app.opts.vertical;
  const int long_side = vert ? app.win_h : app.win_w;
  const int thick = vert ? app.win_w : app.win_h;
  const std::vector<bool> valid = scale_validity(app, long_side);

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

  XFontStruct* font = XLoadQueryFont(app.dpy, "fixed");
  if (!font) font = XLoadQueryFont(app.dpy, "6x13");
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

  // Resize handle on free end — high-contrast grip (visible on light and dark).
  {
    int hx, hy, hw, hh;
    if (vert) {
      hx = 0; hy = long_side - kHandlePx; hw = thick; hh = kHandlePx;
    } else {
      hx = long_side - kHandlePx; hy = 0; hw = kHandlePx; hh = thick;
    }
    // Base fill: amber so it stands out on black and white desktops
    if (app.has_argb && app.pict_format) {
      XRenderColor base = {};
      base.red = 0xe0e0; base.green = 0x9090; base.blue = 0x2020; base.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &base, hx, hy, hw, hh);
      // Light edge
      XRenderColor hi = {};
      hi.red = hi.green = hi.blue = 0xffff; hi.alpha = 0xffff;
      if (vert)
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &hi, hx, hy, hw, 1);
      else
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &hi, hx, hy, 1, hh);
      // Dark edge
      XRenderColor lo = {};
      lo.red = lo.green = lo.blue = 0; lo.alpha = 0xffff;
      if (vert)
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &lo, hx, hy + hh - 1, hw, 1);
      else
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &lo, hx + hw - 1, hy, 1, hh);
      // Grip ridges
      XRenderColor ridge = {};
      ridge.red = 0xffff; ridge.green = 0xd0d0; ridge.blue = 0x8080; ridge.alpha = 0xffff;
      if (vert) {
        for (int i = 2; i < hh - 1; i += 3)
          XRenderFillRectangle(app.dpy, PictOpSrc, pict, &ridge, hx + 2, hy + i, hw - 4, 1);
      } else {
        for (int i = 2; i < hw - 1; i += 3)
          XRenderFillRectangle(app.dpy, PictOpSrc, pict, &ridge, hx + i, hy + 2, 1, hh - 4);
      }
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, 0xe09020);
      XFillRectangle(app.dpy, app.win, app.gc, hx, hy, hw, hh);
      XSetForeground(app.dpy, app.gc, 0xffffff);
      if (vert) XDrawLine(app.dpy, app.win, app.gc, hx, hy, hx + hw - 1, hy);
      else XDrawLine(app.dpy, app.win, app.gc, hx, hy, hx, hy + hh - 1);
      XSetForeground(app.dpy, app.gc, 0x000000);
      if (vert) XDrawLine(app.dpy, app.win, app.gc, hx, hy + hh - 1, hx + hw - 1, hy + hh - 1);
      else XDrawLine(app.dpy, app.win, app.gc, hx + hw - 1, hy, hx + hw - 1, hy + hh - 1);
    }
  }

  // Rotate button at the free end, inset from the resize handle
  int rbx, rby;
  if (vert) {
    rbx = 2;
    rby = long_side - kHandlePx - kRotateBtn - 2;
  } else {
    rbx = long_side - kHandlePx - kRotateBtn - 2;
    rby = 2;
  }
  if (rbx < 2) rbx = 2;
  if (rby < 2) rby = 2;
  // R button: mark fill with contrasting glyph
  {
    Palette pal = current_palette(app);
    if (app.has_argb && app.pict_format) {
      XRenderColor c = {};
      c.red = static_cast<unsigned short>(pal.chip_r * 0x101);
      c.green = static_cast<unsigned short>(pal.chip_g * 0x101);
      c.blue = static_cast<unsigned short>(pal.chip_b * 0x101);
      c.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &c, rbx, rby, kRotateBtn, kRotateBtn);
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, color_chip_bg(app));
      XFillRectangle(app.dpy, app.win, app.gc, rbx, rby, kRotateBtn, kRotateBtn);
    }
  }
  if (font) {
    XSetForeground(app.dpy, app.gc, color_chip_text(app));
    XDrawString(app.dpy, app.win, app.gc, rbx + 5, rby + 13, "R", 1);
  }

  // Unit is included in the free-end length chip (e.g. "30.0 cm"); no extra tag at origin.

  // Total length readout at the FREE END only (never at the origin).
  if (font) {
    std::string len_s = format_length(app);
    const char* lbuf = len_s.c_str();
    int llen = static_cast<int>(len_s.size());
    int ltw = XTextWidth(font, lbuf, llen);
    int pad = 3;
    int lx, ly;
    if (vert) {
      // above the R button / handle cluster
      lx = band_y0 + 2;
      ly = long_side - kHandlePx - kRotateBtn - 18;
    } else {
      // left of R button, still on free end
      lx = long_side - kHandlePx - kRotateBtn - ltw - 10;
      ly = band_y0 + font->ascent + 1;
    }
    if (lx < 2) lx = 2;
    if (ly < font->ascent + 2) ly = font->ascent + 2;
    if (app.has_argb && app.pict_format) {
      Palette pal = current_palette(app);
      XRenderColor chip = {};
      chip.red = static_cast<unsigned short>(pal.chip_r * 0x101);
      chip.green = static_cast<unsigned short>(pal.chip_g * 0x101);
      chip.blue = static_cast<unsigned short>(pal.chip_b * 0x101);
      chip.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &chip,
                           lx - pad, ly - font->ascent - pad,
                           ltw + 2 * pad, font->ascent + font->descent + 2 * pad);
      XRenderFreePicture(app.dpy, pict);
      XSetForeground(app.dpy, app.gc, color_chip_text(app));
    } else {
      XSetForeground(app.dpy, app.gc, color_chip_bg(app));
      XFillRectangle(app.dpy, app.win, app.gc,
                     lx - pad, ly - font->ascent - pad,
                     ltw + 2 * pad, font->ascent + font->descent + 2 * pad);
      XSetForeground(app.dpy, app.gc, color_chip_text(app));
    }
    XDrawString(app.dpy, app.win, app.gc, lx, ly, lbuf, llen);
  }

  // Calibration mode banner — high-contrast so it cannot be missed.
  if (app.calibrating && font) {
    const char* line1 = "CALIBRATION";
    const char* line2 = "Resize to real 30 cm";
    const char* line3 = "Enter=save  Esc=cancel";
    int pad = 4;
    int lh = font->ascent + font->descent + 2;
    int box_h = 3 * lh + 2 * pad;
    int w1 = XTextWidth(font, line1, static_cast<int>(std::strlen(line1)));
    int w2 = XTextWidth(font, line2, static_cast<int>(std::strlen(line2)));
    int w3 = XTextWidth(font, line3, static_cast<int>(std::strlen(line3)));
    int box_w = std::max(std::max(w1, w2), w3) + 2 * pad;
    int bx = std::max(2, (app.win_w - box_w) / 2);
    int by = std::max(2, (app.win_h - box_h) / 2);
    if (app.has_argb && app.pict_format) {
      XRenderColor bg = {};
      bg.red = 0xc000; bg.green = 0x2000; bg.blue = 0x1000; bg.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &bg, bx, by, box_w, box_h);
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, opaque_pixel(app, 192, 32, 16));
      XFillRectangle(app.dpy, app.win, app.gc, bx, by, box_w, box_h);
    }
    XSetForeground(app.dpy, app.gc, color_white(app));
    XDrawString(app.dpy, app.win, app.gc, bx + pad, by + pad + font->ascent, line1,
                static_cast<int>(std::strlen(line1)));
    XDrawString(app.dpy, app.win, app.gc, bx + pad, by + pad + lh + font->ascent, line2,
                static_cast<int>(std::strlen(line2)));
    XDrawString(app.dpy, app.win, app.gc, bx + pad, by + pad + 2 * lh + font->ascent, line3,
                static_cast<int>(std::strlen(line3)));
  }

  // Border
  opaque_line(app, 0, 0, app.win_w - 1, 0);
  opaque_line(app, 0, app.win_h - 1, app.win_w - 1, app.win_h - 1);
  opaque_line(app, 0, 0, 0, app.win_h - 1);
  opaque_line(app, app.win_w - 1, 0, app.win_w - 1, app.win_h - 1);

  if (font) XFreeFont(app.dpy, font);
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

void toggle_orient(App& app) {
  // Keep centre roughly stable, then pull fully on-screen. Without the
  // clamp, a long vertical ruler near the bottom of the display is cut off.
  int cx = app.win_x + app.win_w / 2;
  int cy = app.win_y + app.win_h / 2;
  app.opts.vertical = !app.opts.vertical;
  apply_geometry(app);
  app.win_x = cx - app.win_w / 2;
  app.win_y = cy - app.win_h / 2;
  clamp_position_to_monitor(app);
  XMoveWindow(app.dpy, app.win, app.win_x, app.win_y);
  draw_ruler(app);
}

void begin_calibration(App& app) {
  if (!app.current_monitor) return;
  if (app.calibrating) return;
  app.calibrating = true;
  app.cal_saved_length_mm = app.opts.length_mm;
  app.cal_saved_factor = app.current_monitor->cal_factor;
  app.cal_monitor_name = app.current_monitor->name;
  // Start near the target length so the user only fine-tunes.
  app.opts.length_mm = kCalTargetMm;
  apply_geometry(app);
  draw_ruler(app);
}

void cancel_calibration(App& app) {
  if (!app.calibrating) return;
  app.calibrating = false;
  app.opts.length_mm = app.cal_saved_length_mm;
  // Restore factor on the live monitor entry if still the same output.
  if (app.current_monitor && app.current_monitor->name == app.cal_monitor_name) {
    // Mutate via the vector entry (current_monitor points into monitors).
    for (auto& m : app.monitors) {
      if (m.name == app.cal_monitor_name) {
        m.cal_factor = app.cal_saved_factor;
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
  // Pixel span currently on screen corresponds to a true kCalTargetMm.
  // EDID-based length for the same span (with cal_factor=1) is:
  //   edid_mm = last_tick * edid_axis_mm / axis_px
  // cal_factor = true_mm / edid_mm.
  const bool vert = app.opts.vertical;
  const int span_px = vert ? app.win_h : app.win_w;
  int last_tick = span_px - 1;
  if (last_tick < 1) last_tick = 1;

  // Raw EDID millimetres (ignore current factor).
  const MonitorInfo& m = *app.current_monitor;
  const int raw_mm = vert
      ? ((m.rotation & (RR_Rotate_90 | RR_Rotate_270)) ? m.width_mm : m.height_mm)
      : ((m.rotation & (RR_Rotate_90 | RR_Rotate_270)) ? m.height_mm : m.width_mm);
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
  double factor = kCalTargetMm / edid_mm;
  if (factor < 0.05) factor = 0.05;
  if (factor > 20.0) factor = 20.0;

  app.cal_factors[app.cal_monitor_name] = factor;
  for (auto& mon : app.monitors) {
    if (mon.name == app.cal_monitor_name) {
      mon.cal_factor = factor;
      app.current_monitor = &mon;
      break;
    }
  }
  app.calibrating = false;
  // Keep the window size; length now correctly reports kCalTargetMm.
  app.opts.length_mm = kCalTargetMm;
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
  const bool vert = app.opts.vertical;
  const int long_side = vert ? app.win_h : app.win_w;

  // Resize handle on free end only (right / bottom)
  if (vert) {
    if (y >= long_side - kHandlePx) return App::DragMode::ResizeRight;
  } else {
    if (x >= long_side - kHandlePx) return App::DragMode::ResizeRight;
  }

  // Rotate button next to free-end handle
  int rbx, rby;
  if (vert) {
    rbx = 2;
    rby = long_side - kHandlePx - kRotateBtn - 2;
  } else {
    rbx = long_side - kHandlePx - kRotateBtn - 2;
    rby = 2;
  }
  if (rbx < 2) rbx = 2;
  if (rby < 2) rby = 2;
  if (x >= rbx && x < rbx + kRotateBtn && y >= rby && y < rby + kRotateBtn)
    return App::DragMode::RotateHint;

  // Length chip toggles units on click
  XFontStruct* font = XLoadQueryFont(app.dpy, "fixed");
  if (!font) font = XLoadQueryFont(app.dpy, "6x13");
  if (font) {
    int cx, cy, cw, ch;
    if (length_chip_rect(app, font, &cx, &cy, &cw, &ch)) {
      if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
        XFreeFont(app.dpy, font);
        return App::DragMode::UnitChip;
      }
    }
    XFreeFont(app.dpy, font);
  }

  return App::DragMode::Move;
}

void handle_event(App& app, XEvent& ev) {
  switch (ev.type) {
    case Expose:
      if (ev.xexpose.count == 0) draw_ruler(app);
      break;

    case ConfigureNotify:
      // During free-end resize the origin is locked; ignore WM position/size
      // feedback so the left/top edge cannot jump.
      if (app.drag == App::DragMode::ResizeRight)
        break;
      app.win_x = ev.xconfigure.x;
      app.win_y = ev.xconfigure.y;
      if (ev.xconfigure.width != app.win_w || ev.xconfigure.height != app.win_h) {
        XWindowChanges ch = {};
        ch.x = app.win_x;
        ch.y = app.win_y;
        ch.width = app.win_w;
        ch.height = app.win_h;
        XConfigureWindow(app.dpy, app.win, CWX | CWY | CWWidth | CWHeight, &ch);
      }
      break;

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
          toggle_orient(app);
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
        app.drag_origin_root = app.opts.vertical ? app.win_y : app.win_x;
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
          toggle_orient(app);
        else
          quit(app);
      }
      break;
    }

    case ButtonRelease:
      if (ev.xbutton.button == Button1 || ev.xbutton.button == Button2) {
        const bool was_resize = (app.drag == App::DragMode::ResizeRight);
        app.drag = App::DragMode::Idle;
        // Re-query monitor only after a move; never force a size change after resize.
        if (!was_resize)
          refresh_monitor(app, false);
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
          int span_px = pointer - app.drag_origin_root;
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
        // Ignore other keys while calibrating (q must not quit mid-cal).
        break;
      }
      if (ks == XK_Escape || ks == XK_q)
        quit(app);
      else if (ks == XK_u || ks == XK_U)
        cycle_unit(app);
      else if (ks == XK_r || ks == XK_R)
        toggle_orient(app);
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
  if (!XRRQueryExtension(app.dpy, &event_base, &error_base))
    std::fprintf(stderr, "rulertoo: XRandR not available, using fallback sizes\n");

  create_window(app);
  if (have_saved_pos) {
    // Restore monitor for the saved position, then size + move.
    app.monitors = query_monitors(app.dpy, app.root, app.cal_factors);
    const MonitorInfo* m =
        find_monitor_for_point(app.monitors, app.win_x + 8, app.win_y + 8);
    if (m) {
      app.current_monitor = m;
      app.current_monitor_name = m->name;
    }
    apply_geometry(app);
    clamp_position_to_monitor(app);
    XMoveWindow(app.dpy, app.win, app.win_x, app.win_y);
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
