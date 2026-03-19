# Backend Abstraction Plan

All implementation work described in this document takes place on the
**`feature/backend-abstraction`** branch. Nothing here is merged to `master`
until the full migration checklist at the end of this document is satisfied.

---

## Motivation

AndrathWM currently couples the WM logic, bar rendering, and input handling
tightly to XCB/X11. There are approximately **770 lines containing `xcb_*`
calls** spread across the seven core files:

| File | xcb_* lines |
|---|---|
| `compositor.c` | 278 |
| `client.c` | 133 |
| `awm.c` | 121 |
| `events.c` | 109 |
| `monitor.c` | 51 |
| `systray.c` | 46 |
| `ewmh.c` | 31 |

Additional `xcb_*` usage exists in `compositor_xrender.c` (166),
`switcher.c` (86), `drw.c` (109), `drw_cairo.c` (55), and `xsource.c` (6).
The total across all `src/` files is approximately 1,500 lines.

Runtime state is held in **39 bare `extern` globals** in `awm.h`
(lines 274–312), including the X connection, root window, atoms, keysyms,
DPI constants, and renderer state — all visible to every translation unit.

This makes it impossible to run awm under any display server other than X11
without invasive surgery to core WM logic.

The goal of this refactor is to:

1. Collect all X11 connection state into a single `PlatformCtx` struct.
2. Express every display-server operation that core WM logic needs as a
   `WmBackend` runtime vtable — a struct of function pointers.
3. Express every bar/text drawing operation as a `RenderBackend` runtime
   vtable wrapping the existing `Drw` API.
4. Guard systray and EWMH code (`#ifdef BACKEND_X11`) since they have no
   Wayland equivalents.

The **compositor (EGL/XRender)** is explicitly out of scope. It stays X11-only
for now and will be revisited when wlroots work begins in earnest.

The Wayland target backend is **wlroots** (not raw `libwayland-server`).
Per-monitor fractional DPI is deferred until XLibre exposes fractional scaling
at the protocol level.

---

## Scope

### In scope

- `PlatformCtx` struct: consolidate `xc`, `root`, `screen`, atoms, `keysyms`,
  `numlockmask`, `ui_dpi`, `ui_scale` and the DPI-derived pixel constants
  (`ui_borderpx`, `ui_snap`, `ui_iconsize`, `ui_gappx`) into one struct.
  All call sites changed at once — no alias macros during the transition.
- `WmBackend` runtime vtable: one function pointer per logical WM operation
  (window geometry, focus, stacking, grabs, properties, atom lookups).
- `RenderBackend` + `AwmSurface`: thin vtable wrapping the `Drw` API for bar
  drawing. `drw_cairo.c` becomes `render_cairo_xcb.c`; `drw.c` (legacy) is
  retired.
- `xsource.c` renamed to `platform_x11_source.c`; header to `platform_source.h`.
- Switcher XRender inline path removed: the four helper functions
  (`find_visual_format`, `find_format_for_depth`, `get_root_visualtype`,
  `build_thumbnail`) and the XRender-specific parts of `free_thumbnails`
  (approximately 247 lines, switcher.c lines 138–392 and 616–623) are
  deleted. All thumbnail captures go through `comp_capture_thumb()`.
  `refresh_thumbnail()` already uses `comp_capture_thumb()` and is unchanged.
- `systray.c` and `ewmh.c` guarded with `#ifdef BACKEND_X11`.
- Build system: `BACKEND ?= X11` variable in `config.mk`; `DRW_LEGACY` retired.

### Out of scope

- The compositor (`compositor.c`, `compositor_egl.c`, `compositor_xrender.c`).
  It remains X11-only and is not touched on this branch.
- Any Wayland/wlroots implementation code.
- Per-monitor fractional DPI.
- Session save/restore (Step 7 of the WMState refactor — not yet).

---

## Current vs target layering

```
Current:

  config.h / keybindings
        |
  awm.c / events.c / client.c / monitor.c
        |
  xcb_*() calls (spread everywhere)
        |
  X11 / XCB


Target:

  config.h / keybindings
        |
  awm.c / events.c / client.c / monitor.c
        |
  WmBackend vtable   RenderBackend vtable
        |                    |
  platform_x11.c      render_cairo_xcb.c
        |                    |
        +-------+    +-------+
                |    |
             X11 / XCB
```

---

## New files

| File                        | Purpose                                          |
|-----------------------------|--------------------------------------------------|
| `src/platform.h`            | `PlatformCtx` struct; `WmBackend` vtable typedef |
| `src/platform_x11.c`        | X11 implementation of `WmBackend`                |
| `src/platform_source.h`     | `platform_source_attach()` declaration           |
| `src/platform_x11_source.c` | `xsource.c` renamed; GLib/XCB fd integration    |
| `src/render.h`              | `RenderBackend` vtable typedef; `AwmSurface`     |
| `src/render_cairo_xcb.c`    | `drw_cairo.c` renamed; X11 Cairo implementation |

`drw.c` (legacy XCB+XRender hybrid backend) is removed. `drw.h` / `drw_cairo.c`
are renamed to `render.h` / `render_cairo_xcb.c` in Step 3.

---

## Abstraction 1 — `PlatformCtx`

`src/platform.h`:

```c
/* See LICENSE file for copyright and license details. */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

/* -------------------------------------------------------------------------
 * PlatformCtx — all display-server connection state.
 *
 * Replaces the X11-specific subset of bare extern globals in awm.h:
 *   xc, root, wmcheckwin, screen, sw, sh, bh, lrpad,
 *   wmatom[], netatom[], xatom[], utf8string_atom,
 *   keysyms, numlockmask, ui_dpi, ui_scale,
 *   ui_borderpx, ui_snap, ui_iconsize, ui_gappx.
 *
 * Globals that are NOT moved into PlatformCtx (they are either
 * renderer-owned or WM-logic state, not platform state):
 *   drw, scheme, cursor[]  — owned by RenderBackend after Step 3
 *   systray                — X11-only, stays in systray.c (guarded)
 *   stext, restart, barsdirty — WM logic state, stay in awm.c
 *   launcher_visible, launcher_xwin — UI IPC state, stay in awm.c
 *   last_event_time        — event handling state, stays in awm.c
 *   awm_tagslength         — config-derived constant, stays in awm.c
 *   sniconsize, iconcachesize, iconcachemaxentries, dbustimeout
 *                          — config-derived constants, stay in awm.c
 *   handler[]              — event dispatch table, stays in awm.c
 *
 * One global instance: extern PlatformCtx g_plat;  (defined in platform_x11.c)
 * ---------------------------------------------------------------------- */

#ifdef BACKEND_X11
typedef struct {
    xcb_connection_t    *xc;
    xcb_window_t         root;
    xcb_window_t         wmcheckwin;
    int                  screen;
    int                  sw, sh;          /* screen width / height (px)    */
    int                  bh;              /* bar height (px)               */
    int                  lrpad;           /* sum of left+right font padding */
    xcb_atom_t           wmatom[WMLast];
    xcb_atom_t           netatom[NetLast];
    xcb_atom_t           xatom[XLast];
    xcb_atom_t           utf8string_atom;
    xcb_key_symbols_t   *keysyms;
    unsigned int         numlockmask;
    double               ui_dpi;
    double               ui_scale;
    unsigned int         ui_borderpx;
    unsigned int         ui_snap;
    unsigned int         ui_iconsize;
    unsigned int         ui_gappx;
#ifdef XRANDR
    int randrbase, rrerrbase;
#endif
} PlatformCtx;
#endif /* BACKEND_X11 */

extern PlatformCtx g_plat;

#endif /* PLATFORM_H */
```

Migration strategy: change all call sites in one commit (no alias period).
After Step 2, no `.c` file outside `platform_x11.c` may include `<xcb/xcb.h>`
directly — XCB types reach other files only through `platform.h`.

---

## Abstraction 2 — `WmBackend` runtime vtable

The vtable covers every logical operation the core WM needs from the display
server. The X11 implementation lives in `platform_x11.c`; the future wlroots
implementation will live in `platform_wayland.c`.

```c
typedef struct {
    /* Connection lifecycle */
    int  (*connect)(PlatformCtx *p);
    void (*disconnect)(PlatformCtx *p);
    void (*flush)(PlatformCtx *p);

    /* Atom / property helpers */
    xcb_atom_t (*intern_atom)(PlatformCtx *p, const char *name);
    void (*set_prop_atoms)(PlatformCtx *p, xcb_window_t w,
                           xcb_atom_t prop, xcb_atom_t *vals, int n);
    void (*set_prop_card32)(PlatformCtx *p, xcb_window_t w,
                            xcb_atom_t prop, uint32_t *vals, int n);
    void (*delete_prop)(PlatformCtx *p, xcb_window_t w, xcb_atom_t prop);

    /* Window geometry & stacking */
    void (*configure)(PlatformCtx *p, xcb_window_t w,
                      int x, int y, int bw,
                      unsigned int ww, unsigned int wh);
    void (*restack)(PlatformCtx *p, xcb_window_t w,
                    xcb_window_t sibling, uint32_t stack_mode);
    void (*move_resize)(PlatformCtx *p, xcb_window_t w,
                        int x, int y, unsigned int ww, unsigned int wh);
    void (*set_border_width)(PlatformCtx *p, xcb_window_t w, unsigned int bw);
    void (*set_border_color)(PlatformCtx *p, xcb_window_t w, uint32_t pixel);

    /* Window visibility */
    void (*map)(PlatformCtx *p, xcb_window_t w);
    void (*unmap)(PlatformCtx *p, xcb_window_t w);

    /* Focus & input */
    void (*set_input_focus)(PlatformCtx *p, xcb_window_t w,
                            xcb_timestamp_t t);
    void (*warp_pointer)(PlatformCtx *p, xcb_window_t w,
                         int x, int y);
    void (*grab_button)(PlatformCtx *p, xcb_window_t w,
                        unsigned int mod, unsigned int button);
    void (*ungrab_buttons)(PlatformCtx *p, xcb_window_t w);
    void (*grab_key)(PlatformCtx *p, unsigned int mod, xcb_keycode_t code);
    void (*ungrab_keys)(PlatformCtx *p);

    /* Keyboard symbol lookup */
    xcb_keycode_t *(*get_keycodes)(PlatformCtx *p, uint32_t keysym);
    uint32_t       (*get_keysym)(PlatformCtx *p, xcb_keycode_t code,
                                 unsigned int col);

    /* Cursor */
    xcb_cursor_t (*create_cursor)(PlatformCtx *p, int shape);
    void         (*free_cursor)(PlatformCtx *p, xcb_cursor_t cur);

    /* Monitor / screen geometry */
    void (*update_geom)(PlatformCtx *p);

    /* Send close / kill */
    void (*send_close)(PlatformCtx *p, xcb_window_t w,
                       xcb_timestamp_t t);
    void (*kill_client)(PlatformCtx *p, xcb_window_t w);

    /* Error handler registration */
    void (*set_error_handler)(PlatformCtx *p);
} WmBackend;

extern WmBackend *g_wm_backend;
```

The single global `g_wm_backend` is set in `setup()` before the event loop
starts. No `#ifdef BACKEND_X11` is needed in core logic — the vtable
dispatches transparently.

---

## Abstraction 3 — `RenderBackend` + `AwmSurface`

`src/render.h` replaces `src/drw.h`. The API surface stays almost identical
to the current `Drw` API so diffs are small.

```c
typedef struct AwmSurface AwmSurface;    /* opaque; defined in render_*.c */

typedef struct {
    AwmSurface *(*create)(int w, int h);
    void        (*resize)(AwmSurface *s, int w, int h);
    void        (*free)(AwmSurface *s);

    /* Font */
    Fnt        *(*fontset_create)(AwmSurface *s,
                                  const char **fonts, size_t n);
    void        (*fontset_free)(Fnt *set);
    unsigned int (*fontset_getwidth)(AwmSurface *s, const char *text);

    /* Colour */
    void (*clr_create)(AwmSurface *s, Clr *dest, const char *hex);
    Clr *(*scm_create)(AwmSurface *s, char **names, size_t n);

    /* Cursor */
    Cur *(*cur_create)(AwmSurface *s, int shape);
    void (*cur_free)(AwmSurface *s, Cur *c);

    /* Drawing */
    void (*setscheme)(AwmSurface *s, Clr *scm);
    void (*rect)(AwmSurface *s, int x, int y,
                 unsigned int w, unsigned int h,
                 int filled, int invert);
    int  (*text)(AwmSurface *s, int x, int y,
                 unsigned int w, unsigned int h,
                 unsigned int lpad, const char *txt, int invert);
    void (*pic)(AwmSurface *s, int x, int y,
                unsigned int w, unsigned int h,
                cairo_surface_t *icon);

    /* Flush to window */
    void (*map)(AwmSurface *s, xcb_window_t win,
                int x, int y,
                unsigned int w, unsigned int h);
} RenderBackend;

extern RenderBackend *g_render_backend;
```

`drw_cairo.c` is renamed to `render_cairo_xcb.c` and its `Drw`-typed internal
state is wrapped inside the opaque `AwmSurface` struct. `drw.c` (legacy) is
deleted. `drw.h` is deleted once all references are migrated to `render.h`.

---

## Event dispatch

`xsource.c` is renamed `platform_x11_source.c`. The public header
`xsource.h` becomes `platform_source.h` with a backend-neutral declaration:

```c
/* platform_source.h */
guint platform_source_attach(GMainContext *ctx,
                              GSourceFunc   cb,
                              gpointer      data);
```

The X11 implementation wraps `xcb_get_file_descriptor(g_plat.xc)` exactly
as the current `xsource_attach()` does. A future Wayland implementation
wraps `wl_display_get_fd()`.

---

## Switcher XRender path removal

`switcher.c` contains a full inline XRender thumbnail path spanning
approximately **247 lines** that is X11-specific:

| Block | Lines | Size |
|---|---|---|
| Section divider + `find_visual_format()` | 133–148 | ~16 |
| `find_format_for_depth()` | 151–172 | 22 |
| `get_root_visualtype()` | 174–193 | 20 |
| `build_thumbnail()` | 199–392 | ~194 |
| XRender parts of `free_thumbnails()` | 616–623 | ~8 |

`refresh_thumbnail()` (lines 402–423) has already been migrated to use
`comp_capture_thumb()` and is not touched.

On this branch, the four helper functions and `build_thumbnail` are deleted
entirely. `free_thumbnails()` is reduced to releasing only `thumb_surf`
(the Cairo surface — backend-neutral). If the compositor is not active,
`comp_capture_thumb()` returns `NULL`; the switcher already handles that
case by drawing a placeholder box.

---

## systray and ewmh guards

`systray.c` implements `_NET_SYSTEM_TRAY_S<n>` + `XEMBED` protocol —
inherently X11. There is no Wayland equivalent (SNI/StatusNotifier is
the replacement, already conditionally compiled with `STATUSNOTIFIER`).

`ewmh.c` implements `_NET_*` atoms and ICCCM `WM_*` properties —
also inherently X11.

Both files are wrapped:

```c
#ifdef BACKEND_X11
/* ... entire file contents ... */
#endif /* BACKEND_X11 */
```

A thin `wm_properties.h` header will expose the subset of EWMH/ICCCM
operations that core WM logic calls:

```c
void wmprop_set_active_window(xcb_window_t w);
void wmprop_set_client_list(void);
void wmprop_set_fullscreen(xcb_window_t w, int on);
void wmprop_update_desktop(void);
/* ... */
```

On Wayland these will be no-ops or `xdg-shell` equivalents.

---

## Build system changes

`config.mk` gains a single new knob:

```makefile
BACKEND ?= X11    # X11 | WAYLAND (WAYLAND not yet implemented)
```

Compiler flag emitted: `-DBACKEND_X11` or `-DBACKEND_WAYLAND`.

Source lists split:

```makefile
PLATFORM_SRC = platform_x11.c platform_x11_source.c   # if BACKEND=X11
RENDER_SRC   = render_cairo_xcb.c                      # always for now
```

`DRW_LEGACY=1` is retired. `drw.c` is deleted; `render_cairo_xcb.c` is the
only render backend.

`awm-ui` continues to use `render_cairo_xcb.c` unchanged (it already links
Cairo + Pango against XCB).

---

## Migration plan

Each step is an independent commit. The build must pass with zero warnings
and zero errors after every commit.

### Step 1 — Consolidate X11 globals into `PlatformCtx`

- Add `src/platform.h` with `PlatformCtx` struct (X11 variant, see above).
- Define `PlatformCtx g_plat` in `src/platform_x11.c` (new file, stubs only).
- In `awm.c::setup()`: populate `g_plat` fields from the existing globals,
  then remove the bare `extern` definitions one by one.
- Update every call site that reads `xc`, `root`, `screen`, atoms, `keysyms`,
  `numlockmask`, `ui_dpi`, `ui_scale`, `ui_borderpx`, `ui_snap`, `ui_iconsize`,
  `ui_gappx` to use `g_plat.*`.
- All call sites changed at once — no alias macros.
- The following globals are NOT moved and stay in `awm.h` / `awm.c`:
  `drw`, `scheme`, `cursor[]` (renderer-owned until Step 3);
  `systray` (X11-guarded, Step 6);
  `stext`, `restart`, `barsdirty`, `launcher_visible`, `launcher_xwin`,
  `last_event_time`, `awm_tagslength`, `sniconsize`, `iconcachesize`,
  `iconcachemaxentries`, `dbustimeout`, `handler[]` (WM/IPC state — never
  belong in `PlatformCtx`).

Files touched: `awm.c`, `awm.h`, `client.c`, `events.c`, `monitor.c`,
`ewmh.c`, `compositor.c`, `systray.c`, `switcher.c`, `drw_cairo.c`,
`platform.h` (new), `platform_x11.c` (new).

### Step 2 — Introduce `WmBackend` vtable (X11 implementation)

- Add `WmBackend` typedef to `platform.h`.
- Implement all vtable slots in `platform_x11.c` by lifting the relevant
  `xcb_*` calls out of `client.c`, `monitor.c`, `events.c`.
- Set `g_wm_backend = &wm_backend_x11` in `setup()`.
- Replace direct `xcb_*` calls in core WM files with `g_wm_backend->*()`.
- After this step: `client.c`, `monitor.c`, `events.c` contain no `xcb_*`
  calls; all XCB is in `platform_x11.c` and `ewmh.c`/`systray.c` (guarded).

### Step 3 — Rename `drw` → `render`; introduce `RenderBackend`

- Rename `drw.h` → `render.h`, `drw_cairo.c` → `render_cairo_xcb.c`.
- Add `RenderBackend` vtable and opaque `AwmSurface` type to `render.h`.
- Wrap `Drw` inside `AwmSurface` in `render_cairo_xcb.c`.
- Update all `drw_*()` call sites to `g_render_backend->*()`.
- Move `drw`, `scheme`, `cursor[]` ownership to `RenderBackend` / `AwmSurface`;
  remove their `extern` declarations from `awm.h`.
- Delete `drw.c` (legacy XCB+XRender backend).
- Update `Makefile` / `config.mk`: retire `DRW_LEGACY`, add `RENDER_SRC`.

### Step 4 — Rename `xsource` → `platform_source`

- Rename `xsource.c` → `platform_x11_source.c`,
  `xsource.h` → `platform_source.h`.
- Rename `xsource_attach()` → `platform_source_attach()`.
- Update `awm.c` and `Makefile`.

### Step 5 — Remove inline XRender path from `switcher.c`

- Delete `find_visual_format()`, `find_format_for_depth()`,
  `get_root_visualtype()`, `build_thumbnail()` (lines 133–392), and the
  `thumb_pict` / `thumb_pixmap` cleanup in `free_thumbnails()` (lines 616–623).
- Remove `thumb_pixmap` and `thumb_pict` fields from `SwitcherEntry`.
- Verify placeholder-box path still works when compositor is inactive.

### Step 6 — Guard `systray.c` and `ewmh.c`; add `wm_properties.h`

- Wrap both files in `#ifdef BACKEND_X11`.
- Extract the narrow `wmprop_*` interface into `src/wm_properties.h`.
- Provide stub `wm_properties_x11.c` (calls through to existing `ewmh.c`
  functions) and an empty `wm_properties_stub.c` for future non-X11 builds.
- Add `BACKEND ?= X11` to `config.mk`; emit `-DBACKEND_X11`.

---

## Invariants

In addition to all rules in `AGENTS.md`, the following hold on this branch:

- After Step 2: no `xcb_*` call appears in `client.c`, `monitor.c`, or
  `events.c`. XCB is only in `platform_x11.c`, `ewmh.c`, `systray.c`,
  `compositor*.c`.
- After Step 3: no `drw_*` call appears outside `render_cairo_xcb.c`.
  `drw.h` and `drw.c` do not exist.
- After Step 4: no `#include "xsource.h"` anywhere. Only
  `#include "platform_source.h"`.
- After Step 5: no `xcb_render_*`, `xcb_composite_*`, `xcb_create_pixmap`,
  or `xcb_free_pixmap` calls appear in `switcher.c`. `thumb_pict` and
  `thumb_pixmap` fields do not exist in `SwitcherEntry`.
- After Step 6: no `xcb_intern_atom`, `xcb_change_property`, or
  `xcb_get_property` call appears outside `platform_x11.c`,
  `ewmh.c` (guarded), or `systray.c` (guarded).
- `compositor_backend.h` remains internal to the three compositor files.
- `config.h` remains the last `#include` in every `.c` file that uses it.
- Zero `#include <X11/...>` introduced anywhere.
- Zero bare `printf` / `fprintf` for diagnostics.
- Build produces zero warnings and zero errors at every commit.

---

## What this branch does NOT do

- No Wayland code is written.
- No wlroots API calls.
- No compositor changes.
- No session save/restore (Step 7 of the WMState refactor).
- No per-monitor fractional DPI.
- No changes to `ui_proto.h` or the awm-ui IPC protocol.
- No changes to `config.h` user-visible options.

---

## Future work (post-merge, separate branch)

| Item                           | File(s)                                        |
|--------------------------------|------------------------------------------------|
| Wayland platform layer         | `src/platform_wayland.c`                       |
| Wayland event source           | `src/platform_wayland_source.c`                |
| wlroots scene-graph render     | `src/render_cairo_wl.c`                        |
| xdg-shell property stubs       | `src/wm_properties_xdg.c`                     |
| SNI systray replacement        | extend existing `sni.c` / `STATUSNOTIFIER`     |
| Session save/restore           | `src/session.c` (WMState Step 7)               |

---

## Merge checklist

Before opening a pull request from `feature/backend-abstraction` to `master`:

- [x] `make` produces zero warnings and zero errors (`-Werror -Wall -pedantic`)
- [x] `make DRW_LEGACY=1` removed from CI — flag no longer exists
- [x] No `#include <X11/...>` in any `src/` file
- [x] No `xcb_*` function calls in `client.c`, `monitor.c`, `events.c` (grep clean; bootstrap xcb_connect/xcb_disconnect in main() exempt)
- [x] No `drw_*` outside `render_cairo_xcb.c` (grep clean)
- [x] No `#include "xsource.h"` anywhere
- [x] `systray.c` and `ewmh.c` wrapped in `#ifdef BACKEND_X11`
- [ ] `config.h` is last `#include` in every `.c` that uses it
- [x] Inline XRender path removed from `switcher.c`; `thumb_pict` and
      `thumb_pixmap` fields removed from `SwitcherEntry`
- [x] All `awm_*` logging macros used — no bare `printf`/`fprintf`
- [ ] `AGENTS.md` constraints checklist satisfied in full
- [ ] Tested in Xephyr: `Xephyr :1 -screen 1280x720 && DISPLAY=:1 ./awm -s`
