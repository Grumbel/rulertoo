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
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kDefaultLengthMm = 300;  // 30 cm
constexpr int kRulerHeightPx = 48;     // fixed pixel height of the strip
constexpr double kBgAlpha = 0.45;      // background alpha when ARGB available

struct MonitorInfo {
  int x = 0, y = 0;
  int width_px = 0, height_px = 0;
  int width_mm = 0, height_mm = 0;
  std::string name;
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

  int length_mm = kDefaultLengthMm;
  int win_x = 100, win_y = 100;
  int win_w = 0, win_h = kRulerHeightPx;

  bool dragging = false;
  int drag_off_x = 0, drag_off_y = 0;

  std::vector<MonitorInfo> monitors;
  const MonitorInfo* current_monitor = nullptr;

  Atom atom_wm_delete = 0;
  Atom atom_net_wm_state = 0;
  Atom atom_net_wm_state_above = 0;
  Atom atom_motif_hints = 0;
};

void die(const char* msg) {
  std::fprintf(stderr, "x11-ruler: %s\n", msg);
  std::exit(1);
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
    m.name = oi->name ? oi->name : "unknown";
    if (m.width_mm > 0 && m.height_mm > 0 && m.width_px > 0) {
      out.push_back(m);
    }
    XRRFreeCrtcInfo(ci);
    XRRFreeOutputInfo(oi);
  }
  XRRFreeScreenResources(res);

  if (out.empty()) {
    MonitorInfo m;
    m.x = 0;
    m.y = 0;
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
        py >= m.y && py < m.y + m.height_px) {
      return &m;
    }
  }
  if (mons.empty()) return nullptr;
  const MonitorInfo* best = &mons[0];
  long best_d = 1L << 30;
  for (const auto& m : mons) {
    int cx = m.x + m.width_px / 2;
    int cy = m.y + m.height_px / 2;
    long dx = px - cx, dy = py - cy;
    long d = dx * dx + dy * dy;
    if (d < best_d) {
      best_d = d;
      best = &m;
    }
  }
  return best;
}

double px_per_mm(const MonitorInfo& m) {
  if (m.width_mm <= 0 || m.width_px <= 0) return 96.0 / 25.4;
  return static_cast<double>(m.width_px) / static_cast<double>(m.width_mm);
}

void set_borderless(App& app) {
  struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
  } hints = {};
  hints.flags = 2;          // MWM_HINTS_DECORATIONS
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
  double ppm = app.current_monitor ? px_per_mm(*app.current_monitor) : (96.0 / 25.4);
  app.win_w = static_cast<int>(std::lround(app.length_mm * ppm));
  app.win_h = kRulerHeightPx;

  app.win = XCreateWindow(app.dpy, app.root, app.win_x, app.win_y, app.win_w,
                          app.win_h, 0, app.depth, InputOutput, app.visual,
                          valuemask, &swa);

  app.atom_wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(app.dpy, app.win, &app.atom_wm_delete, 1);
  app.atom_net_wm_state = XInternAtom(app.dpy, "_NET_WM_STATE", False);
  app.atom_net_wm_state_above = XInternAtom(app.dpy, "_NET_WM_STATE_ABOVE", False);
  app.atom_motif_hints = XInternAtom(app.dpy, "_MOTIF_WM_HINTS", False);

  XStoreName(app.dpy, app.win, "x11-ruler");
  set_borderless(app);

  Atom type = XInternAtom(app.dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom utility = XInternAtom(app.dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(app.dpy, app.win, type, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&utility), 1);

  app.gc = XCreateGC(app.dpy, app.win, 0, nullptr);

  if (app.has_argb) {
    app.pict_format = XRenderFindVisualFormat(app.dpy, app.visual);
  }

  XMapWindow(app.dpy, app.win);
  XFlush(app.dpy);
}

void update_size_for_monitor(App& app) {
  if (!app.current_monitor) return;
  double ppm = px_per_mm(*app.current_monitor);
  int new_w = static_cast<int>(std::lround(app.length_mm * ppm));
  if (new_w < 50) new_w = 50;
  if (new_w != app.win_w) {
    app.win_w = new_w;
    XResizeWindow(app.dpy, app.win, app.win_w, app.win_h);
  }
}

void refresh_monitor(App& app) {
  app.monitors = query_monitors(app.dpy, app.root);
  int cx = app.win_x + app.win_w / 2;
  int cy = app.win_y + app.win_h / 2;
  const MonitorInfo* m = find_monitor_for_point(app.monitors, cx, cy);
  if (m != app.current_monitor) {
    app.current_monitor = m;
    update_size_for_monitor(app);
  } else if (m) {
    update_size_for_monitor(app);
  }
}

void draw_ruler(App& app) {
  if (!app.current_monitor) return;
  double ppm = px_per_mm(*app.current_monitor);

  if (app.has_argb && app.pict_format) {
    XRenderColor color = {};
    unsigned short a = static_cast<unsigned short>(kBgAlpha * 0xffff);
    color.alpha = a;
    color.red = static_cast<unsigned short>(0xe0 * kBgAlpha * 0xff);
    color.green = static_cast<unsigned short>(0xe8 * kBgAlpha * 0xff);
    color.blue = static_cast<unsigned short>(0xf0 * kBgAlpha * 0xff);
    Picture pict = XRenderCreatePicture(app.dpy, app.win, app.pict_format, 0, nullptr);
    XRenderFillRectangle(app.dpy, PictOpSrc, pict, &color, 0, 0, app.win_w, app.win_h);
    XRenderFreePicture(app.dpy, pict);
  } else {
    XSetForeground(app.dpy, app.gc, 0xd0d8e0);
    XFillRectangle(app.dpy, app.win, app.gc, 0, 0, app.win_w, app.win_h);
  }

  XSetForeground(app.dpy, app.gc, BlackPixel(app.dpy, app.screen));

  const int bottom = app.win_h - 2;
  const int full_h = bottom - 2;
  const int mid_h = full_h * 2 / 3;
  const int short_h = full_h / 3;

  XDrawLine(app.dpy, app.win, app.gc, 0, bottom, app.win_w, bottom);

  XFontStruct* font = XLoadQueryFont(app.dpy, "fixed");
  if (!font) font = XLoadQueryFont(app.dpy, "6x13");
  if (font) XSetFont(app.dpy, app.gc, font->fid);

  for (int mm = 0; mm <= app.length_mm; ++mm) {
    int x = static_cast<int>(std::lround(mm * ppm));
    if (x < 0 || x >= app.win_w) continue;

    int h = short_h;
    if (mm % 10 == 0) {
      h = full_h;
    } else if (mm % 5 == 0) {
      h = mid_h;
    }
    XDrawLine(app.dpy, app.win, app.gc, x, bottom - h, x, bottom);

    if (mm % 10 == 0 && font) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%d", mm / 10);
      int tw = XTextWidth(font, buf, static_cast<int>(std::strlen(buf)));
      int tx = x - tw / 2;
      if (tx < 1) tx = 1;
      if (tx + tw > app.win_w - 1) tx = app.win_w - 1 - tw;
      XDrawString(app.dpy, app.win, app.gc, tx, bottom - full_h - 2, buf,
                  static_cast<int>(std::strlen(buf)));
    }
  }

  if (font) XFreeFont(app.dpy, font);

  XDrawRectangle(app.dpy, app.win, app.gc, 0, 0, app.win_w - 1, app.win_h - 1);

  XFlush(app.dpy);
}

void handle_event(App& app, XEvent& ev) {
  switch (ev.type) {
    case Expose:
      if (ev.xexpose.count == 0) draw_ruler(app);
      break;
    case ConfigureNotify:
      app.win_x = ev.xconfigure.x;
      app.win_y = ev.xconfigure.y;
      if (ev.xconfigure.width != app.win_w || ev.xconfigure.height != app.win_h) {
        XResizeWindow(app.dpy, app.win, app.win_w, app.win_h);
      }
      refresh_monitor(app);
      break;
    case ButtonPress:
      if (ev.xbutton.button == Button1) {
        app.dragging = true;
        app.drag_off_x = ev.xbutton.x;
        app.drag_off_y = ev.xbutton.y;
      } else if (ev.xbutton.button == Button3) {
        XUnmapWindow(app.dpy, app.win);
        XDestroyWindow(app.dpy, app.win);
        XCloseDisplay(app.dpy);
        std::exit(0);
      }
      break;
    case ButtonRelease:
      if (ev.xbutton.button == Button1) {
        app.dragging = false;
        refresh_monitor(app);
        draw_ruler(app);
      }
      break;
    case MotionNotify:
      if (app.dragging) {
        int nx = ev.xmotion.x_root - app.drag_off_x;
        int ny = ev.xmotion.y_root - app.drag_off_y;
        app.win_x = nx;
        app.win_y = ny;
        XMoveWindow(app.dpy, app.win, nx, ny);
        refresh_monitor(app);
      }
      break;
    case KeyPress: {
      KeySym ks = XLookupKeysym(&ev.xkey, 0);
      if (ks == XK_Escape || ks == XK_q) {
        XUnmapWindow(app.dpy, app.win);
        XDestroyWindow(app.dpy, app.win);
        XCloseDisplay(app.dpy);
        std::exit(0);
      }
      break;
    }
    case ClientMessage:
      if (static_cast<Atom>(ev.xclient.data.l[0]) == app.atom_wm_delete) {
        XUnmapWindow(app.dpy, app.win);
        XDestroyWindow(app.dpy, app.win);
        XCloseDisplay(app.dpy);
        std::exit(0);
      }
      break;
    default:
      break;
  }
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  App app;
  app.dpy = XOpenDisplay(nullptr);
  if (!app.dpy) die("cannot open X display");

  int event_base, error_base;
  if (!XRRQueryExtension(app.dpy, &event_base, &error_base)) {
    std::fprintf(stderr, "x11-ruler: XRandR not available, using fallback sizes\n");
  }

  create_window(app);
  refresh_monitor(app);
  draw_ruler(app);

  XEvent ev;
  while (true) {
    XNextEvent(app.dpy, &ev);
    handle_event(app, ev);
  }
  return 0;
}
