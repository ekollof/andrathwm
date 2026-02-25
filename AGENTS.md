# AGENTS.md — AndrathWM (awm) Coding Agent Reference

## Build Commands

```sh
make                  # build awm, awm-ui, xidle (default Cairo backend)
make DRW_LEGACY=1     # build with legacy XCB+Cairo hybrid backend (deprecated — will be removed on feature/backend-abstraction)
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
1. System/standard headers (`<stdint.h>`, `<stdlib.h>`, `<string.h>`, ...)
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
`syslog`. **Never use bare `printf` or `fprintf(stderr, ...)` for diagnostics** — always use
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
Monitors are stored as a **flat array** `g_awm.monitors[WMSTATE_MAX_MONITORS]` with
`g_awm.n_monitors` tracking the count and `g_awm.selmon_num` (int index) identifying the
focused monitor. Use the following macros instead of direct field access:

```c
g_awm_selmon            /* expands to &g_awm.monitors[g_awm.selmon_num] */
g_awm_set_selmon(m)     /* sets selmon_num from a Monitor * pointer */
FOR_EACH_MON(var)       /* replaces for(m=mons; m; m=m->next) loops */
```

`Monitor` has no `next` pointer — iteration is always via `FOR_EACH_MON`. `updategeom()`
queries RandR/Xinerama and populates the flat array. Per-tag layout state lives inline in
`Monitor.pertag` (a value sub-struct, not a pointer).

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
- `capture_thumb(cw, max_w, max_h)` -> `cairo_surface_t *` — render a scaled thumbnail:
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

### IPC (awm <-> awm-ui)
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

## WMState Refactor — Active Development Goal

### Motivation

The WM state is currently a sea of ~30 bare `extern` globals scattered across `awm.c`
(`mons`, `selmon`, `cl`, `stext`, `restart`, `barsdirty`, `comp.*`, etc.).  Every module
reaches into these globals directly, making it easy to accidentally break invariants, hard
to reason about ownership, and impossible to atomically manipulate state.

### Primary Goal (current)

**Consolidate restartable/observable WM and compositor state into a single `AWMState`
struct (`g_awm`).**  The immediate payoff is disambiguation and simplicity — not
serialisation.

- `AWMState` is the single source of truth for monitor topology, per-tag layout state,
  client state, compositor redirect/bypass state, and focused monitor/client.
- Functions access shared state via the single global `g_awm` rather than a bag of
  unrelated globals.
- State transitions become explicit: it is obvious which fields change and why.
- Invariants are enforced at the struct boundary, not scattered across call sites.

### What belongs in AWMState

| Field group | Contents |
|---|---|
| Monitor topology | `monitors[WMSTATE_MAX_MONITORS]` flat array, `n_monitors`, `selmon_num` |
| Per-tag layout | Inline `Pertag` sub-struct per `Monitor`: `nmasters`, `mfacts`, `sellts`, `ltidxs`, `showbars`, `drawwithgaps`, `gappx`, `curtag`, `prevtag` |
| Client state | Per-client: `win`, `tags`, `mon`, `x/y/w/h`, `opacity`, `isfloating`, `isfullscreen`, `ishidden`, `scratchkey`, `issteam`, `bypass_compositor` |
| Focus | `selmon_num` index; per-monitor `sel` pointer |
| Compositor | `comp.paused_mask`, per-window redirect/bypass state |

### What does NOT belong in AWMState (runtime-only)

Rendering resources (`drw`, `scheme`, `cursor`), atom caches (`wmatom`, `netatom`,
`xatom`), input state (`keysyms`, `numlockmask`), IPC fds (`ui_fd`, `ui_pid`), X
connection (`xc`, `root`, `screen`).  These are setup once and never need to be
snapshotted or restored.

### Implementation plan (feature/state-refactor branch)

**Step 1 (done on master, commit `838bae0`):** Three independent bug fixes.

**Step 2 (done on master, commit `4eec58d`):**
Replace `Monitor` cached fields with `MON_*` accessor macros.

**Step 3 — AWMState struct skeleton (done):**
Defined `AWMState` in `src/wmstate.h`, global `AWMState g_awm` in `awm.c`,
populated in `setup()`. Old `extern` globals removed incrementally.

**Step 4 — Migrate modules to use `g_awm` (done):**
`monitor.c`, `client.c`, `events.c`, `compositor.c` migrated in per-module commits.

**Step 5 — Compositor state into AWMState (done, commit `9e67b4b`):**
`comp.paused_mask` snapshotted into `AWMState`; `AWMStateComp` dual-writes removed.

**Step 6 — Monitor flat array (done, commit `86e9238`):**
Replace `AWMState.mons` linked list + `AWMState.selmon` pointer with
`AWMState.monitors[]` flat array + `AWMState.selmon_num` index.

- `Monitor` no longer has a `next` pointer.
- Access via `g_awm_selmon`, `g_awm_set_selmon(m)`, `FOR_EACH_MON(var)` macros.
- `Pertag` is now an inline value sub-struct inside `Monitor` (not a pointer).
- Files already migrated: `src/wmstate.h`, `src/awm.h`, `src/monitor.c`, `src/wmstate.c`.
- Files still pending migration (all `g_awm.mons` / `g_awm.selmon` sites):
  - `src/awm.c`
  - `src/client.c`
  - `src/events.c`
  - `src/ewmh.c`
  - `src/compositor.c`
  - `src/switcher.c`

**Step 7 (future, not yet):** Serialisation — once `AWMState` is the single source of
truth, session save/restore and JSON dump become a single walk of `g_awm`. Do not
implement serialisation until Step 6 is complete and stable.

### Flat array migration — key patterns

```c
/* Old linked-list iteration */
for (m = g_awm.mons; m; m = m->next) { ... }

/* New flat-array iteration — declare Monitor *m before the loop */
Monitor *m;
FOR_EACH_MON(m) { ... }

/* Old selmon read */
g_awm.selmon->field

/* New */
g_awm_selmon->field

/* Old selmon write */
g_awm.selmon = m;

/* New */
g_awm_set_selmon(m);

/* Old "more than one monitor" check (tagmon etc.) */
g_awm.mons->next != NULL

/* New */
g_awm.n_monitors > 1

/* Old selmon null-guard (ui_send_monitor_geom) */
if (!g_awm.selmon || ...)

/* New */
if (g_awm.selmon_num < 0 || ...)

/* Old cleanup loop */
while (g_awm.mons) cleanupmon(g_awm.mons);

/* New */
while (g_awm.n_monitors) cleanupmon(&g_awm.monitors[0]);
```

### Rules for this refactor

- **Do not implement session save/restore or JSON dump yet.**  That is Step 7.
- **Do not break the build between commits.**  Migrate incrementally.
- **Do not introduce a `AWMState *` parameter to every function** — use the
  single `g_awm` global during the transition.
- `src/session.c`, `src/session.h`, `src/state_dump.c`, `src/state_dump.h` introduced on
  the old `feature/state-refactor` branch are **not carried forward** — they were
  premature and will be rewritten in Step 7.

---

## Backend Abstraction — Incoming Change

**Branch: `feature/backend-abstraction`** (not yet started; planned after WMState Step 7)

The backend abstraction refactor decouples WM logic, bar rendering, and input
handling from XCB/X11 so that a future wlroots/Wayland backend can be added
without invasive changes to core WM files.  Full design in
`docs/BACKEND_ABSTRACTION.md`.

### What changes

| Area | Change |
|---|---|
| `PlatformCtx` struct | All ~30 `extern` globals (`xc`, `root`, atoms, `keysyms`, DPI) move into a single `g_plat` struct defined in `platform_x11.c`. All call sites updated at once — no alias period. |
| `WmBackend` vtable | Runtime struct of function pointers for every logical WM operation. After migration `client.c`, `monitor.c`, `events.c` contain zero `xcb_*` calls. |
| `RenderBackend` vtable | Thin wrapper over the `Drw` API. `drw_cairo.c` → `render_cairo_xcb.c`. `drw.c` (legacy) deleted. `drw.h` → `render.h`. |
| `platform_source` | `xsource.c` → `platform_x11_source.c`; `xsource.h` → `platform_source.h`; `xsource_attach()` → `platform_source_attach()`. |
| Switcher | Inline XRender thumbnail path (~80 lines) removed from `switcher.c`; all captures route through `comp_capture_thumb()`. |
| systray / ewmh | Both files wrapped in `#ifdef BACKEND_X11`. Narrow `wmprop_*` interface extracted to `wm_properties.h`. |
| Build system | `BACKEND ?= X11` in `config.mk`; emits `-DBACKEND_X11`. `DRW_LEGACY=1` flag removed. |

### What does NOT change on this branch

- The compositor (`compositor.c`, `compositor_egl.c`, `compositor_xrender.c`)
  stays X11-only and is not touched.
- No Wayland or wlroots code is written.
- No per-monitor fractional DPI.
- No session save/restore (WMState Step 7 — separate concern).
- No changes to `ui_proto.h` or the awm-ui IPC protocol.

### New files

| File | Purpose |
|---|---|
| `src/platform.h` | `PlatformCtx` struct; `WmBackend` vtable typedef |
| `src/platform_x11.c` | X11 implementation of `WmBackend`; defines `g_plat` |
| `src/platform_source.h` | `platform_source_attach()` declaration |
| `src/platform_x11_source.c` | `xsource.c` renamed |
| `src/render.h` | `RenderBackend` vtable; `AwmSurface` opaque type |
| `src/render_cairo_xcb.c` | `drw_cairo.c` renamed |

### Rules for this refactor

- All work on `feature/backend-abstraction` — never directly on `master`.
- Build must pass with zero warnings/errors after every commit.
- All `g_plat.*` migration done in one commit — no alias macros during transition.
- After Step 2: `client.c`, `monitor.c`, `events.c` must be grep-clean of `xcb_*`.
- After Step 3: no `drw_*` call outside `render_cairo_xcb.c`; `drw.h`/`drw.c` deleted.
- Compositor files are not modified on this branch.
- No Wayland code introduced on this branch.

---

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
- [ ] WMState refactor: no session/dump code introduced before Step 7
- [ ] No `Monitor *next` field used anywhere — use `FOR_EACH_MON` instead
- [ ] No direct `g_awm.mons` or `g_awm.selmon` access — use macros
- [ ] Backend abstraction: on `feature/backend-abstraction` only — no vtable/platform code on `master` before the branch merges
