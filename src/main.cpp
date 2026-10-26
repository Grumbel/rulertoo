// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kDefaultLengthMm = 300;
constexpr int kDefaultHeightPx = 52;
constexpr double kDefaultBgAlpha = 0.40;
constexpr int kHandlePx = 10;          // free-end (right/bottom) resize handle
constexpr int kRotateBtn = 18;         // size of rotate button area

enum class Unit { Mm, Cm, Inch, Pixel };

struct MonitorInfo {
  int x = 0, y = 0;
  int width_px = 0, height_px = 0;
  int width_mm = 0, height_mm = 0;  // physical panel axes (native)
  int rotation = 0;                  // RR_Rotate_* bitmask from CRTC
  std::string name;

  // Physical millimetres along the *current* screen X / Y axes
  // (accounts for 90/270° CRTC rotation).
  int mm_along_x() const {
    if (rotation & (RR_Rotate_90 | RR_Rotate_270))
      return height_mm;
    return width_mm;
  }
  int mm_along_y() const {
    if (rotation & (RR_Rotate_90 | RR_Rotate_270))
      return width_mm;
    return height_mm;
  }
};

struct Options {
  int length_mm = kDefaultLengthMm;
  int height_px = kDefaultHeightPx;
  double bg_alpha = kDefaultBgAlpha;
  Unit unit = Unit::Cm;
  bool vertical = false;
  bool help = false;
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
  int drag_root_x = 0, drag_root_y = 0;     // root grab pos (resize / origin)
  int drag_start_length = 0;
  int drag_start_origin = 0;
  int origin_mm = 0;          // numbering origin shift (mm)

  std::vector<MonitorInfo> monitors;
  const MonitorInfo* current_monitor = nullptr;
  std::string current_monitor_name;

  Atom atom_wm_delete = 0;
  Atom atom_net_wm_state = 0;
  Atom atom_net_wm_state_above = 0;
  Atom atom_motif_hints = 0;
};

void die(const char* msg) {
  std::fprintf(stderr, "rulertoo: %s\n", msg);
  std::exit(1);
}

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
      "  -h, --help          Show this help\n"
      "\n"
      "Controls:\n"
      "  Left-drag           Move the ruler\n"
      "  Drag free end         Resize length (right/bottom handle)\n"
      "  Middle-drag        Shift numbering origin\n"
      "  Right-click handle   Toggle orientation (or R button / key r)\n"
      "  u                  Cycle units (cm -> mm -> in -> px)\n"
      "  r                  Rotate horizontal/vertical\n"
      "  Esc / q            Quit\n"
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
    } else if (!std::strcmp(a, "-l") || !std::strcmp(a, "--length")) {
      o.length_mm = std::atoi(need("--length"));
      if (o.length_mm < 20) o.length_mm = 20;
      if (o.length_mm > 2000) o.length_mm = 2000;
    } else if (!std::strcmp(a, "-H") || !std::strcmp(a, "--height")) {
      o.height_px = std::atoi(need("--height"));
      if (o.height_px < 28) o.height_px = 28;
      if (o.height_px > 200) o.height_px = 200;
    } else if (!std::strcmp(a, "-a") || !std::strcmp(a, "--alpha")) {
      o.bg_alpha = std::atof(need("--alpha"));
      if (o.bg_alpha < 0.0) o.bg_alpha = 0.0;
      if (o.bg_alpha > 1.0) o.bg_alpha = 1.0;
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
    } else if (!std::strcmp(a, "-v") || !std::strcmp(a, "--vertical")) {
      o.vertical = true;
    } else {
      std::fprintf(stderr, "rulertoo: unknown option: %s\n", a);
      print_usage(argv[0]);
      std::exit(2);
    }
  }
  return o;
}

std::vector<MonitorInfo> query_monitors(Display* dpy, Window root) {
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

double px_per_mm(const MonitorInfo& m, bool vertical) {
  // CRTC width/height are already in the current orientation (and reflect
  // xrandr --scale when the server reports the scaled mode size).
  // Physical mm must follow the same axis after rotation.
  if (vertical) {
    int mm = m.mm_along_y();
    if (mm <= 0 || m.height_px <= 0) return 96.0 / 25.4;
    return static_cast<double>(m.height_px) / static_cast<double>(mm);
  }
  int mm = m.mm_along_x();
  if (mm <= 0 || m.width_px <= 0) return 96.0 / 25.4;
  return static_cast<double>(m.width_px) / static_cast<double>(mm);
}

void set_borderless(App& app) {
  struct {
    unsigned long flags, functions, decorations;
    long input_mode;
    unsigned long status;
  } hints = {};
  hints.flags = 2;
  hints.decorations = 0;
  if (app.atom_motif_hints) {
    XChangeProperty(app.dpy, app.win, app.atom_motif_hints, app.atom_motif_hints,
                    32, PropModeReplace, reinterpret_cast<unsigned char*>(&hints), 5);
  }
  if (app.atom_net_wm_state && app.atom_net_wm_state_above) {
    XChangeProperty(app.dpy, app.win, app.atom_net_wm_state, XA_ATOM, 32,
                    PropModeReplace,
                    reinterpret_cast<unsigned char*>(&app.atom_net_wm_state_above), 1);
  }
}

void apply_geometry(App& app) {
  if (!app.current_monitor) return;
  double ppm = px_per_mm(*app.current_monitor, app.opts.vertical);
  int long_px = static_cast<int>(std::lround(app.opts.length_mm * ppm));
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
  XResizeWindow(app.dpy, app.win, app.win_w, app.win_h);
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
  swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | StructureNotifyMask | KeyPressMask;
  unsigned long valuemask = CWColormap | CWBorderPixel | CWBackPixel | CWEventMask;

  app.monitors = query_monitors(app.dpy, app.root);
  app.current_monitor = app.monitors.empty() ? nullptr : &app.monitors[0];
  if (app.current_monitor)
    app.current_monitor_name = app.current_monitor->name;

  double ppm = app.current_monitor
                   ? px_per_mm(*app.current_monitor, app.opts.vertical)
                   : (96.0 / 25.4);
  int long_px = static_cast<int>(std::lround(app.opts.length_mm * ppm));
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
  set_borderless(app);

  Atom type = XInternAtom(app.dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom utility = XInternAtom(app.dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(app.dpy, app.win, type, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&utility), 1);

  app.gc = XCreateGC(app.dpy, app.win, 0, nullptr);
  if (app.has_argb)
    app.pict_format = XRenderFindVisualFormat(app.dpy, app.visual);

  XMapWindow(app.dpy, app.win);
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

  app.monitors = query_monitors(app.dpy, app.root);
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

void fill_bg(App& app) {
  if (app.has_argb && app.pict_format) {
    XRenderColor color = {};
    unsigned short a = static_cast<unsigned short>(app.opts.bg_alpha * 0xffff);
    color.alpha = a;
    color.red   = static_cast<unsigned short>(0xd0 * app.opts.bg_alpha * 0xff);
    color.green = static_cast<unsigned short>(0xd8 * app.opts.bg_alpha * 0xff);
    color.blue  = static_cast<unsigned short>(0xe0 * app.opts.bg_alpha * 0xff);
    Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
    XRenderFillRectangle(app.dpy, PictOpSrc, pict, &color, 0, 0, app.win_w, app.win_h);
    XRenderFreePicture(app.dpy, pict);
  } else {
    XSetForeground(app.dpy, app.gc, 0xc8d0d8);
    XFillRectangle(app.dpy, app.win, app.gc, 0, 0, app.win_w, app.win_h);
  }
}

void opaque_rect(App& app, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (app.has_argb && app.pict_format) {
    XRenderColor black = {0, 0, 0, 0xffff};
    Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
    XRenderFillRectangle(app.dpy, PictOpSrc, pict, &black, x, y, w, h);
    XRenderFreePicture(app.dpy, pict);
  } else {
    XSetForeground(app.dpy, app.gc, BlackPixel(app.dpy, app.screen));
    XFillRectangle(app.dpy, app.win, app.gc, x, y, w, h);
  }
}

void opaque_line(App& app, int x1, int y1, int x2, int y2) {
  if (x1 == x2)
    opaque_rect(app, x1, std::min(y1, y2), 1, std::abs(y2 - y1) + 1);
  else if (y1 == y2)
    opaque_rect(app, std::min(x1, x2), y1, std::abs(x2 - x1) + 1, 1);
  else {
    XSetForeground(app.dpy, app.gc, BlackPixel(app.dpy, app.screen));
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
  const int mm = app.opts.length_mm;
  char buf[48];
  switch (app.opts.unit) {
    case Unit::Mm:
      std::snprintf(buf, sizeof(buf), "%d mm", mm);
      break;
    case Unit::Cm:
      std::snprintf(buf, sizeof(buf), "%.1f cm", mm / 10.0);
      break;
    case Unit::Inch:
      std::snprintf(buf, sizeof(buf), "%.2f in", mm / 25.4);
      break;
    case Unit::Pixel: {
      double ppm = app.current_monitor
                       ? px_per_mm(*app.current_monitor, app.opts.vertical)
                       : (96.0 / 25.4);
      std::snprintf(buf, sizeof(buf), "%.0f px", mm * ppm);
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
  fill_bg(app);

  double ppm = px_per_mm(*app.current_monitor, app.opts.vertical);
  const bool vert = app.opts.vertical;
  const int long_side = vert ? app.win_h : app.win_w;
  const int thick = vert ? app.win_w : app.win_h;

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

  // Opaque centre band so numbers stay readable on any desktop background
  if (vert) {
    // vertical: band along X in the middle of thickness
    if (app.has_argb && app.pict_format) {
      XRenderColor band = {};
      band.red = 0xffff; band.green = 0xfaf0; band.blue = 0xe8e0; band.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &band, band_y0, 0, band_h, long_side);
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, 0xfff2d0);
      XFillRectangle(app.dpy, app.win, app.gc, band_y0, 0, band_h, long_side);
    }
  } else {
    if (app.has_argb && app.pict_format) {
      XRenderColor band = {};
      band.red = 0xffff; band.green = 0xfaf0; band.blue = 0xe8e0; band.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &band, 0, band_y0, long_side, band_h);
      XRenderFreePicture(app.dpy, pict);
    } else {
      XSetForeground(app.dpy, app.gc, 0xfff2d0);
      XFillRectangle(app.dpy, app.win, app.gc, 0, band_y0, long_side, band_h);
    }
  }

  if (font) XSetFont(app.dpy, app.gc, font->fid);

  for (int mm = 0; mm <= app.opts.length_mm; ++mm) {
    int pos = static_cast<int>(std::lround(mm * ppm));
    if (pos < 0 || pos >= long_side) continue;

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
      char buf[24];
      if (app.opts.unit == Unit::Inch)
        std::snprintf(buf, sizeof(buf), "%.1f", val);
      else if (app.opts.unit == Unit::Pixel)
        std::snprintf(buf, sizeof(buf), "%.0f", val);
      else if (app.opts.unit == Unit::Cm)
        std::snprintf(buf, sizeof(buf), "%.0f", val);
      else
        std::snprintf(buf, sizeof(buf), "%d", rel);

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
        XRenderColor chip = {};
        // Opaque near-white paper (16-bit channels, full alpha)
        chip.red = 0xffff; chip.green = 0xf8f8; chip.blue = 0xe8e8; chip.alpha = 0xffff;
        Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
        XRenderFillRectangle(app.dpy, PictOpSrc, pict, &chip, cx, cy, cw, ch);
        XRenderFreePicture(app.dpy, pict);
      } else {
        XSetForeground(app.dpy, app.gc, 0xfff8e8);
        XFillRectangle(app.dpy, app.win, app.gc, cx, cy, cw, ch);
      }
      XSetForeground(app.dpy, app.gc, BlackPixel(app.dpy, app.screen));
      XDrawString(app.dpy, app.win, app.gc, tx, ty, buf, len);
    }
  }

  // Resize handle on free end (right / bottom)
  if (vert)
    opaque_rect(app, 0, long_side - kHandlePx, thick, kHandlePx);
  else
    opaque_rect(app, long_side - kHandlePx, 0, kHandlePx, thick);

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
  opaque_rect(app, rbx, rby, kRotateBtn, kRotateBtn);
  if (font) {
    XSetForeground(app.dpy, app.gc, WhitePixel(app.dpy, app.screen));
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
      XRenderColor chip = {};
      chip.red = 0x2020; chip.green = 0x2020; chip.blue = 0x2020; chip.alpha = 0xffff;
      Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
      XRenderFillRectangle(app.dpy, PictOpSrc, pict, &chip,
                           lx - pad, ly - font->ascent - pad,
                           ltw + 2 * pad, font->ascent + font->descent + 2 * pad);
      XRenderFreePicture(app.dpy, pict);
      XSetForeground(app.dpy, app.gc, WhitePixel(app.dpy, app.screen));
    } else {
      XSetForeground(app.dpy, app.gc, 0x202020);
      XFillRectangle(app.dpy, app.win, app.gc,
                     lx - pad, ly - font->ascent - pad,
                     ltw + 2 * pad, font->ascent + font->descent + 2 * pad);
      XSetForeground(app.dpy, app.gc, WhitePixel(app.dpy, app.screen));
    }
    XDrawString(app.dpy, app.win, app.gc, lx, ly, lbuf, llen);
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

void toggle_orient(App& app) {
  // Keep centre roughly stable
  int cx = app.win_x + app.win_w / 2;
  int cy = app.win_y + app.win_h / 2;
  app.opts.vertical = !app.opts.vertical;
  apply_geometry(app);
  app.win_x = cx - app.win_w / 2;
  app.win_y = cy - app.win_h / 2;
  XMoveWindow(app.dpy, app.win, app.win_x, app.win_y);
  draw_ruler(app);
}

void quit(App& app) {
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
      app.win_x = ev.xconfigure.x;
      app.win_y = ev.xconfigure.y;
      // During our own resize we already set win_w/win_h; ignore transient WM sizes.
      if (app.drag == App::DragMode::ResizeRight)
        break;
      if (ev.xconfigure.width != app.win_w || ev.xconfigure.height != app.win_h)
        XResizeWindow(app.dpy, app.win, app.win_w, app.win_h);
      break;

    case ButtonPress: {
      int bx = ev.xbutton.x;
      int by = ev.xbutton.y;
      // Ignore scroll wheel (often reported as Button4/5); it was shifting
      // origin / clearing labels when mis-handled as middle interactions.
      if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
        break;
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
        // ResizeRight / Move / Origin handled in MotionNotify
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
        int lx = ev.xmotion.x;
        int ly = ev.xmotion.y;

        if (app.drag == App::DragMode::Move) {
          int nx = mx - app.drag_off_x;
          int ny = my - app.drag_off_y;
          app.win_x = nx;
          app.win_y = ny;
          XMoveWindow(app.dpy, app.win, nx, ny);
          if (refresh_monitor(app, false))
            draw_ruler(app);
        } else if (app.drag == App::DragMode::ResizeRight) {
          if (!app.current_monitor) break;
          // Derive length from origin edge → pointer in root space so the
          // result is independent of intermediate window sizes.
          double ppm = px_per_mm(*app.current_monitor, app.opts.vertical);
          int origin_root = app.opts.vertical ? app.win_y : app.win_x;
          int pointer = app.opts.vertical ? my : mx;
          int long_px = pointer - origin_root;
          if (long_px < 60) long_px = 60;
          int new_len = static_cast<int>(std::lround(long_px / ppm));
          if (new_len < 30) new_len = 30;
          if (new_len > 2000) new_len = 2000;
          if (new_len != app.opts.length_mm) {
            app.opts.length_mm = new_len;
            apply_geometry(app);
            draw_ruler(app);
          }
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
      if (ks == XK_Escape || ks == XK_q)
        quit(app);
      else if (ks == XK_u || ks == XK_U)
        cycle_unit(app);
      else if (ks == XK_r || ks == XK_R)
        toggle_orient(app);
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

  app.dpy = XOpenDisplay(nullptr);
  if (!app.dpy) die("cannot open X display");

  int event_base, error_base;
  if (!XRRQueryExtension(app.dpy, &event_base, &error_base))
    std::fprintf(stderr, "rulertoo: XRandR not available, using fallback sizes\n");

  create_window(app);
  refresh_monitor(app, true);
  draw_ruler(app);

  XEvent ev;
  while (true) {
    XNextEvent(app.dpy, &ev);
    handle_event(app, ev);
  }
  return 0;
}
