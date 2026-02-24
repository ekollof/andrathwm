# AGENTS.md — AndrathWM (awm) Coding Agent Reference

## Build Commands

```sh
make                  # build awm, awm-ui, xidle (default Cairo backend)
make DRW_LEGACY=1     # build with legacy XCB+Cairo hybrid backend
make clean            # remove all build artefacts
make install          # install to ${PREFIX}/bin (default /usr/bin)
make compdb           # generate compile_commands.json for clangd
make compile_flags    # regenerate compile_flags.txt for LSP
```

There is no test suite. To run awm in a nested X session:

```sh
Xephyr :1 -screen 1280x720 &
sleep 1 && DISPLAY=:1 ./awm          # XRender compositor fallback activates
DISPLAY=:1 ./awm -s                  # skip autostart scripts
```

**Compiler:** `clang` (canonical). The build must produce **zero warnings and zero errors**
(`-Werror -Wall -pedantic -std=c11`).

To enable debug logging, uncomment in `config.mk`:
```makefile
CPPFLAGS += -DAWM_DEBUG
```

## Code Style

AndrathWM follows **suckless K&R style** strictly.

### Indentation & Braces
- **Tabs** for indentation (width 4). Never spaces for indentation.
- **K&R braces** for control flow — opening brace on same line.
- **Allman for function definitions** — return type on its own line, opening brace on the next:

```c
void
arrange(Monitor *m)
{
    if (m)
        showhide(m->cl->stack);
}
```

- Single-statement `if`/`for`/`while` bodies without braces are fine.
- Line limit: **79 characters** where practical.

### Naming Conventions

| Category          | Convention            | Examples                                    |
|-------------------|-----------------------|---------------------------------------------|
| Functions         | `snake_case`          | `applyrules`, `createmon`, `comp_add_win`   |
| Types / Structs   | `PascalCase`          | `Client`, `Monitor`, `CompWin`, `CompShared`|
| Enum constants    | `PascalCase`          | `CurNormal`, `SchemeNorm`, `ColFg`          |
| Macros            | `ALL_CAPS`            | `MODKEY`, `TAGMASK`, `ISVISIBLE`, `WIDTH`   |
| Local variables   | short lowercase       | `m`, `c`, `cw`, `bh`, `sw`, `sh`           |
| C99 integer types | for protocol widths   | `uint8_t`, `uint32_t`, `int32_t`            |
| Boolean           | plain `int`, 0/1      | never `bool`, `Bool`, `True`, `False`       |

### Comments
- Block comments only: `/* ... */`. No `//` in production code.
- File header (first line of every `.c`/`.h`):
  `/* See LICENSE file for copyright and license details. */`
- Section dividers:
  ```c
  /* -------------------------------------------------------------------------
   * Section title
   * ---------------------------------------------------------------------- */
  ```
- Inline comments aligned to the right with tabs.

### Alignment
Vertically align consecutive assignments and declarations in declaration blocks:

```c
typedef union {
    int          i;
    unsigned int ui;
    float        f;
    const void  *v;
} Arg;
```

Pointer stars bind to the name: `char *p`, not `char* p`.

## Include / Header Conventions

**`config.h` must be the last `#include` in any `.c` file that uses it.**

Only `src/awm.c` defines `AWM_CONFIG_IMPL` before including it:
```c
#define AWM_CONFIG_IMPL
#include "config.h"
```

Standard include order within a `.c` file:
1. System/standard headers (`<stdint.h>`, `<stdlib.h>`, `<string.h>`, …)
2. Third-party library headers (XCB, GLib, Cairo, Pango, GTK, EGL/GL)
3. Project headers (`"awm.h"`, then peer module headers)
4. `"config.h"` — always last

**Never reorder includes.** `.clang-format` has `SortIncludes: Never`; include order is
load-order significant.

`compositor_backend.h` is **internal to the compositor** — do not include it from any file
outside `compositor.c`, `compositor_egl.c`, `compositor_xrender.c`.

## X11 / XCB Rules

- **No Xlib.** Zero `#include <X11/...>` anywhere in `src/`. All X11 communication goes
  through the global `xcb_connection_t *xc` (declared `extern` in `awm.h`).
- Free XCB reply memory with `free()`, never `XFree()`.
- Use `XKB_KEY_*` for keysyms, `XCB_MOD_MASK_*` for modifiers, `XCB_BUTTON_INDEX_*` for
  buttons. Never `XK_*`, `ShiftMask`, `Mod1Mask`, `Button1`.
- `XCB_CONFIG_WINDOW_*` value arrays must be ordered by ascending bit position.
- `xcb_key_symbols_get_keycode()` returns a `malloc`'d array terminated by `XCB_NO_SYMBOL`
  — always `free()` it.
- `xcb_randr_get_screen_resources_crtcs(sr)` points into the reply buffer — use it before
  `free(sr)`.
- `xcb_icccm_wm_hints_t.flags` is `int32_t`; `xcb_size_hints_t.flags` is `uint32_t`.
- Pango font strings use Pango description format: `"BerkeleyMono Nerd Font 12"`, not Xft
  patterns.

## Logging

Four macros, defined in `src/log.h` (implemented in `src/log.c`):

```c
awm_debug("msg %d", val);  /* compile-time no-op unless -DAWM_DEBUG */
awm_info("msg %d", val);
awm_warn("msg %d", val);
awm_error("msg %d", val);
```

All four automatically inject `__func__` and `__LINE__`. Output goes to both `stderr` and
`syslog`. **Never use bare `printf` or `fprintf(stderr, …)` for diagnostics** — always use
these macros.

For signal handlers use the async-signal-safe macros from `util.h` (`LOG_SAFE`,
`LOG_SAFE_ERR`, `LOG_SAFE_WARN`) which use `write()` with string literals only.

## Error Handling

- **Fatal errors:** `die("context:")` from `util.h`. If the format string ends with `:`,
  `die()` appends `perror(NULL)` automatically. Use this for `errno`-producing failures:
  ```c
  if (!(p = calloc(nmemb, size)))
      die("calloc:");
  ```
- **Memory allocation:** Prefer `ecalloc(nmemb, size)` — it calls `die` on failure, so the
  return value never needs a NULL check.
- **Non-fatal errors:** Log with `awm_error()` or `awm_warn()` and return early or continue.
- **XCB async errors:** `xcb_error_handler()` in `events.c` whitelists known-benign races
  (BadWindow, BadMatch, BadDrawable, BadAccess, BadIDChoice). The WM must survive them.
- **XCB checked requests:** Use `xcb_*_checked()` + `xcb_request_check()` only when the
  error must be caught synchronously (e.g., probing extension availability, `checkotherwm`).

## Key Architectural Patterns

### Event Loop
GLib main loop (`gtk_main()`). X events arrive via `XSource` (a custom `GSource` watching
the XCB fd in `src/xsource.c`). The dispatch callback `x_dispatch_cb()` drains
`xcb_poll_for_event()` in a loop, passing events first to `compositor_handle_event()` then
to the `handler[]` table indexed by `response_type & ~0x80`.

### Clients
`Client` structs live in two linked lists per monitor: `cl->clients` (creation order) and
`cl->stack` (focus/MRU order). `manage()` on `MapRequest`, `unmanage()` on
`DestroyNotify`/`UnmapNotify`.

### Monitors
`mons` is a linked list of `Monitor`; `selmon` is focused. `updategeom()` queries RandR/
Xinerama. Per-tag layout state lives in `Monitor.pertag`.

### Compositor
Backend vtable pattern: `CompBackend` struct of function pointers. Two singletons:
`comp_backend_egl` (EGL/GL, tried first) and `comp_backend_xrender` (fallback). Shared
state in global `CompShared comp`. Per-window state in the `CompWin` linked list
(`comp.windows`). Repaints are idle-scheduled, vblank-timed via X Present.

Fullscreen bypass is per-monitor via `comp.paused_mask` (bitmask, bit N = monitor N
bypassed). `comp.paused` is only `1` when **all** monitors are bypassed. XShape holes are
punched in the overlay per bypassed monitor by `comp_update_overlay_shape()`.

`compositor_backend.h` defines `CompWin`, `CompBackend` vtable, and `CompShared`. It is
**internal** — never include it outside `compositor.c`, `compositor_egl.c`,
`compositor_xrender.c`.

Key vtable entries in `CompBackend`:
- `bind_pixmap(cw)` — build EGLImageKHR+GL texture (EGL) or XRender Picture (XRender)
- `release_pixmap(cw)` — free the above; safe to call with no binding held
- `repaint()` — execute one full composite pass
- `capture_thumb(cw, max_w, max_h)` → `cairo_surface_t *` — render a scaled thumbnail:
  EGL path uses GL FBO + `glReadPixels`; XRender path composites to a temp pixmap + `xcb_get_image`
- `notify_resize()` — handle screen geometry change
- `apply_shape(cw)` — apply ShapeBounding clip (may be NULL)

Public thumbnail API (called from the switcher):
```c
cairo_surface_t *comp_capture_thumb(Client *c, int max_w, int max_h);
```
Dispatches through the backend vtable. Caller owns the returned surface and must
`cairo_surface_destroy()` it.

**Tag-swap + bypass invariant:** `compositor_check_unredirect()` must be called after
`arrange(m)` for *each* monitor involved in a tag swap — not only via `focus()` at the end.
`attachclients()` reassigns `c->mon` before `check_unredirect()` runs, so the `removed` path
uses window screen coordinates (centre-point in monitor rect) to determine which monitor a
window lives on, **not** `cw->client->mon`.

### Window Switcher
`src/switcher.c` / `src/switcher.h`. Alt+Tab / Super+Tab overlay implemented inside the
`awm` process with direct access to `Client` lists and compositor textures.

Public API:
```c
void switcher_init(void);          /* call once from setup() */
void switcher_show(const Arg *);   /* arg->i=0 current monitor, 1=all */
void switcher_show_prev(const Arg *);
void switcher_next(const Arg *);
void switcher_prev(const Arg *);
void switcher_confirm_xkb(const Arg *);  /* called on Alt/Super release */
void switcher_cancel_xkb(const Arg *);  /* called on Escape */
void switcher_cleanup(void);
int  switcher_active(void);        /* guard in enternotify/focusin */
```

Key design points:
- GTK `override_redirect` window; horizontal `GtkBox` of `GtkDrawingArea` cards.
- **All key routing via awm's XCB handlers** (`keypress`/`keyrelease`), not GTK events.
- Keyboard grabbed via `xcb_grab_keyboard` on root while overlay is visible; released and
  confirmed on Alt/Super key-release.
- `switcher_active()` must be checked in `enternotify()` and `focusin()` to prevent awm
  from stealing focus while the user is cycling.
- Thumbnails refreshed on a 100 ms `g_timeout_add` timer via `comp_capture_thumb()`.

Config bindings (in `config.h`):
```c
{ XCB_MOD_MASK_1,              XKB_KEY_Tab, switcher_show,      {.i=0} },
{ XCB_MOD_MASK_1|XCB_MOD_MASK_SHIFT, XKB_KEY_Tab, switcher_show_prev, {.i=0} },
{ MODKEY,                      XKB_KEY_Tab, switcher_show,      {.i=1} },
{ MODKEY|XCB_MOD_MASK_SHIFT,   XKB_KEY_Tab, switcher_show_prev, {.i=1} },
```

### IPC (awm ↔ awm-ui)
`awm-ui` is a separate process hosting GTK popups (launcher, SNI menus). Communication is
via `SOCK_SEQPACKET` socketpair. Protocol: fixed `UiMsgHeader` (type + payload_len) followed
by typed payload structs defined in `src/ui_proto.h`.

### x11_constants.h
`src/x11_constants.h` contains the handful of X11 protocol constants awm needs that have no
direct XCB equivalent:
- `KeySym` typedef (`uint32_t`) — guarded with `#ifndef X_H` to avoid conflict when
  `<gdk/gdkx.h>` pulls in `<X11/X.h>` transitively.
- `LASTEvent 36` — size of the `handler[]` dispatch table.
- `X_ConfigureWindow`, `X_GrabButton`, `X_GrabKey`, `X_SetInputFocus`, `X_CopyArea`,
  `X_PolySegment`, `X_PolyFillRectangle`, `X_PolyText8` — core request opcodes used in
  `xcb_error_handler()` to classify and whitelist async errors.

## Constraints Checklist

Before committing, verify:

- [ ] `make` produces zero warnings and zero errors
- [ ] `config.h` is the last `#include` in every modified `.c` file
- [ ] No `#include <X11/...>` introduced
- [ ] No Xlib types (`Window`, `Atom`, `Display *`, `Bool`, `True`, `False`)
- [ ] XCB reply memory freed with `free()`, not `XFree()`
- [ ] No bare `printf`/`fprintf` for diagnostics — use `awm_*` logging macros
- [ ] `AWM_CONFIG_IMPL` defined only in `src/awm.c`
- [ ] New fields added to `CompShared` or other shared structs do not silently shift offsets
  used by existing code in companion `.c` files (check all `comp_backend_*` files compile)
