# XCB Migration — Branch `xcb-migration`

**Goal:** Replace Xlib round-trip calls in `awm`'s core source files with their XCB
equivalents, while leaving intentional Xlib keeps in place (see below).  The build must
stay clean under `clang -std=c11 -pedantic -Werror -Wall -Wno-deprecated-declarations`.

---

## Build

```sh
make clean && make          # must produce zero warnings/errors
sudo make install
```

XCB is accessed through the Xlib-XCB bridge:

```c
xcb_connection_t *xc = XGetXCBConnection(dpy);
```

`<X11/Xlib-xcb.h>` is included via `src/awm.h`.  All XCB reply memory is freed with
`free()`, not `XFree()`.

---

## Commits landed (oldest → newest)

| Hash | What was migrated |
|------|-------------------|
| `65b1d95` | Batch-intern all atoms at startup via async XCB cookies (`XInternAtom` → `xcb_intern_atom`) |
| `33c4d8e` | Pure-flush `XSync`/`XFlush` → `xcb_flush()` via `xflush()` macro |
| `642b398` | `XChangeProperty` / `XDeleteProperty` / `XGetWindowProperty` → XCB across all core files |
| `a062f00` | Window attribute/geometry calls in `client.c` |
| `f2fa8da` | `XSelectInput` → `xcb_change_window_attributes(XCB_CW_EVENT_MASK)` |
| `8ec66b1` | `XSendEvent` in `configure()` and `sendevent()` |
| `d3cdb23` | `events.c` window management |
| `54360df` | `monitor.c` window management |
| `869f925` | `compositor.c` window management |
| `6e58136` | `systray.c` window management |
| `3a2bda7` | `client.c` remaining window management |
| `7e7e3ce` | `ewmh.c`: `XGetWMProtocols`, `XSetInputFocus` |
| `90bf747` | Geometry, grab/warp, class hint, keyboard mapping, visual walk |
| `b45e0c7` | WM hints (`xcb-icccm`), size hints, keyboard (`xcb-keysyms`), RandR (`xcb-randr`), `compositor.c` `XGetWindowAttributes` |
| `b7bd833` | `menu.c` and `sni.c`: window/grab/RandR/keysym calls migrated to XCB |
| `ef9ad51` | `launcher.c`: window/grab/keysym calls migrated to XCB; `XLookupString` kept |
| `e6d4f72` | `drw.c`/`drw.h`: replace Xft text rendering with PangoCairo; remove `drw_font_getexts`; `config.mk` updated |
| `be054a8` | `compositor.c`: `XGetWindowProperty` (two sites) → `xcb_get_property`; `XDestroyWindow` → `xcb_destroy_window` |
| `892ad40` | `XGrabServer`/`XUngrabServer` → `xcb_grab_server`/`xcb_ungrab_server`; remove `xerrordummy` |
| `6f355e1` | `XGetTransientForHint` → `xcb_icccm_get_wm_transient_for_reply` in `client.c` |
| `abf186b` | `XQueryTree` → `xcb_query_tree`; `XAddToSaveSet` → `xcb_change_save_set` |
| `f4e8125` | `systray.c`: `XSetSelectionOwner`/`XGetSelectionOwner` → `xcb_set_selection_owner`/`xcb_get_selection_owner` |
| `a2de4f0` | `monitor.c`: Xinerama → `xcb_xinerama_*`; `isuniquegeom()` updated; `-lxcb-xinerama` added to `config.mk` |
| `120a687` | `menu.c`: Xinerama block → `xcb_xinerama_is_active_reply`/`xcb_xinerama_query_screens_reply`; include updated |
| `5611caa` | `ewmh.c` `setdesktopnames()`: `Xutf8TextListToTextProperty`/`XSetTextProperty` → `xcb_change_property` with NUL-separated UTF-8 blob |
| `a8ce948` | `client.c` `gettextprop()`: `XGetTextProperty`/`XmbTextPropertyToTextList`/`XFree` → `xcb_icccm_get_text_property` |
| `032ee8a` | `compositor.c`: `XInternAtom` (4×) → `xcb_intern_atom`; `XSetSelectionOwner`/`XGetSelectionOwner` → xcb; `XQueryTree` → `xcb_query_tree`; `XGetGeometry` → `xcb_get_geometry`; `XQueryExtension` → `xcb_get_extension_data` |
| `f349de2` | `compositor.c`: complete XCB migration — all XComposite/XDamage/XFixes/XRender/XShape calls replaced; `damage_ring[]` / `dirty_get_bbox` types changed to `xcb_rectangle_t`; `config.mk` COMPOSITORLIBS updated |
| `c42cf25` | Replace `XVisualIDFromVisual(DefaultVisual(dpy, screen))` with XCB screen setup walk at all sites: `xcb_screen_root_visual()` static inline helper added to `awm.h`; used in `compositor.c` (4×), `menu.c`, `launcher.c`; `drw.c` inlines the walk directly (no `awm.h` include) |
| `9cf01d0` | `xidle`: replace all Xlib/XScreenSaver with `xcb-screensaver` — `xcb_connect`, `xcb_get_extension_data`, `xcb_screensaver_query_info`, `free`; `Makefile` xidle rule updated to `-lxcb -lxcb-screensaver` |
| `3587a02` | `xrdb.c`: replace `XOpenDisplay`/`XResourceManagerString`/`XrmGetStringDatabase`/`XCloseDisplay` with `xcb_connect`/`xcb_intern_atom`/`xcb_get_property` on `RESOURCE_MANAGER` root property; `xrdb_lookup()` static helper for suffix key matching; `awm.c`: remove `XrmInitialize()`; `awm.h`: remove `<X11/Xresource.h>` and `XRDB_LOAD_COLOR` macro; `events.c`: `checkotherwm()` probe replaced with `xcb_change_window_attributes_checked`+`xcb_request_check`; `xerrorstart()` removed |
| `ebfa6b6` | Phase 1 batch: `ConnectionNumber` → `xcb_get_file_descriptor(XGetXCBConnection())` in `spawn.c`, `xsource.c`, `launcher.c`; `XSupportsLocale` removed from `awm.c`; `XRRUpdateConfiguration` removed, `RRScreenChangeNotify` → `XCB_RANDR_SCREEN_CHANGE_NOTIFY`; `DefaultScreen`/`DisplayWidth`/`DisplayHeight`/`RootWindow` → XCB setup roots iterator in `awm.c`; `awm.h`: `Xrandr.h`/`Xinerama.h` removed; `launcher.c`: `DefaultDepth` → `xs->root_depth`, `DefaultScreen`/`DisplayWidth`/`DisplayHeight` → XCB screen walk, `XLookupString` → `xkb_keysym_to_utf8`; `drw.c`: `XParseColor`/`XAllocColor`/`DefaultColormap` → sscanf hex parser + `xcb_alloc_color`; `compositor.c`: all 15 `xerror_push_ignore`/`XSync`/`xerror_pop` triplets replaced with `_checked`+`xcb_request_check` or `xcb_flush`; push/pop infrastructure deleted; `config.mk`: add `-lxkbcommon` |
| `beaeb24` | Phase 2 complete: all 15 `handler[]` callbacks (`events.c/h`) rewritten from `void(*)(XEvent*)` to `void(*)(xcb_generic_event_t*)`; `x_dispatch_cb` (`awm.c`) replaced `XPending`/`XNextEvent` with `xcb_poll_for_event` drain loop; `movemouse` + `resizemouse` (`client.c`) replaced `XMaskEvent` with `xcb_wait_for_event` for-loop and `XPutBackEvent` drain with `xcb_poll_for_event` dispatch; `monitor.c` both `XPutBackEvent` sites replaced with dispatch loops; `compositor_handle_event` + `comp_repaint_idle` (`compositor.c`) fully rewritten with XCB types; `systray.h/c`, `launcher.h/c`, `menu.h/c`, `sni.h/c` handler signatures updated to `xcb_generic_event_t*`; `XPending` removed from `xsource.c` (both `prepare` and `check`); dead `XWindowChanges wc` removed from `manage()` and `resizeclient()` |
| `615729e` | Phase 3b complete: replace global `Display *dpy` with `xcb_connection_t *xc` across all TUs — `awm.c/h`: `xcb_connect`/`xcb_disconnect`, `extern xcb_connection_t *xc`; `client.c`: remove local `xc` self-assign, fix `gettextprop`; `compositor.c`: remove Xlib headers, fix `pext` variable corruption, remove self-assigns; `drw.c/h`: `drw_create` shim accepts `xc`, opens private `Display*` for Phase 3c scaffolding; `events.c`, `ewmh.c`, `monitor.c`, `systray.c`: self-assigns removed, `DefaultDepth`/`DefaultScreen` replaced; `launcher.c/h`, `menu.c/h`, `xsource.c/h`: full `xcb_connection_t *xc` migration; `sni.c/h`: `sni_init` takes `xc`, local `xcb_screen_root_depth_sni` helper, `extern int screen`; `spawn.c`: replace `dpy` with `xc` in child fork fd-close guard |
| `23cf5a7` | Phase 3c complete: `drw.h`/`drw.c` fully rewritten to pure XCB+xcb-renderutil (no Xlib); `awm.h` drops Xlib-xcb bridge and Xlib extension headers; `config.mk` fixes `xcb-renderutil` pkg-config name; `menu.h/c`, `sni.h/c`, `systray.h/c`, `launcher.h/c`: `Window`→`xcb_window_t`, `Time`→`xcb_timestamp_t`, `Pixmap`→`xcb_pixmap_t`, `Button[123]`→`1/2/3`, `CurrentTime`→`XCB_CURRENT_TIME` throughout; orphaned `SNIIconLoadData` struct and stale placeholder body removed from `sni.c` |
| `016c377` | Phase 4 complete: eliminate all remaining Xlib types — `Window`→`xcb_window_t`, `Atom`→`xcb_atom_t`, `Pixmap`→`xcb_pixmap_t`, `Time`→`xcb_timestamp_t`, `CurrentTime`→`XCB_CURRENT_TIME`, `None`→`XCB_ATOM_NONE`/`0` across `awm.h`, `awm.c`, `client.h/c`, `ewmh.h/c`, `events.c`, `monitor.h/c`, `compositor.c`, `systray.c`; `manage()` takes `xcb_get_geometry_reply_t*` directly; `sendevent()` fully typed as XCB |
| `875085b` | Phase 5 complete: eliminate all remaining Xlib symbolic constants, dead headers and `Bool`/`True`/`False` — `awm.h`: drop `X11/Xatom.h`, `X11/Xutil.h`, `X11/keysym.h`, `X11/X.h`, `X11/extensions/scrnsaver.h` (XSS block); add `<xkbcommon/xkbcommon-keysyms.h>`; `CLEANMASK` uses `XCB_MOD_MASK_*`; `config.def.h`+`config.h`: `XK_*`→`XKB_KEY_*`, `ShiftMask`/`ControlMask`/`Mod[1-5]Mask`/`LockMask`→`XCB_MOD_MASK_*`, `MODKEY`/`ALTKEY` → `XCB_MOD_MASK_4`/`XCB_MOD_MASK_1`; `awm.c`: cursor integer literals replace `XC_*`, drop `X11/cursorfont.h`, all event masks → `XCB_EVENT_MASK_*`, `IsViewable`→`XCB_MAP_STATE_VIEWABLE`, `IconicState`→`XCB_ICCCM_WM_STATE_ICONIC`; `client.c`: `Bool`/`True`/`False`→`int`/`1`/`0`, `NoEventMask`→`0`, `XA_WM_NAME`→`XCB_ATOM_WM_NAME`, WM state/event mask consts replaced; `ewmh.c`: `True`/`False`→`1`/`0`, `NoEventMask`→`0`; `events.c`: `X11/XKBlib.h`→`xkbcommon-keysyms.h`, `XK_Num_Lock`→`XKB_KEY_Num_Lock`, all `XA_*`/state/event mask consts replaced; `monitor.c`: `False`→`0`, event masks replaced; `systray.c`: `False`→`0`, WM state + event mask consts replaced; `compositor.c`: `None`→`0` on pixmap/picture fields, event masks replaced; `menu.c`+`launcher.c`: `X11/keysym.h`→`xkbcommon-keysyms.h`, `XK_*`→`XKB_KEY_*` |
| `dfb7153` | Phase 6 — replace Xlib `xerror()`/`XSetErrorHandler` with pure XCB error handler; drop all Xlib link deps (`libX11`, `libXinerama`, `libXrandr`, `libXss`, `libXcomposite`, `libXdamage`, `libXrender`, `libXfixes`, `libXext`, `libX11-xcb`) removed from `config.mk`; `X11INC`/`X11LIB` path vars removed; `xcb_error_handler()` + `xcb_error_text()` replace `xerror()` in `events.c/h`; wired into `x_dispatch_cb()` for `response_type == 0` errors |
| `1343b5d`, `7d8e76d` | Phase 6b — replace `<X11/X.h>` and `<X11/Xproto.h>` with XCB names and new `src/x11_constants.h`; all X11 event type constants (`ButtonPress`, `KeyPress`, `MapNotify`, etc.) → `XCB_BUTTON_PRESS`, `XCB_KEY_PRESS`, `XCB_MAP_NOTIFY`, etc. throughout `awm.c` and `compositor.c`; `LockMask` → `XCB_MOD_MASK_LOCK` in `client.c` and `events.c`; `ResizeRedirectMask` → `XCB_EVENT_MASK_RESIZE_REDIRECT` in `events.c`; `SelectionClear` → `XCB_SELECTION_CLEAR` in `compositor.c`; `Button1`–`Button5` → `XCB_BUTTON_INDEX_1`–`XCB_BUTTON_INDEX_3` in `config.h` and `config.def.h`; `x11_constants.h` now contains only `KeySym` typedef, `LASTEvent`, and the eight `X_` request opcodes (which XCB has no equivalent for) |
| `9912d55` | Audit fix: `drw_fontset_getwidth_clamp` `invert` arg corrected (was passing `n` where `0` was required); `xcb-migration.md` updated to reflect completed migration |
| `af0e300` | **Phase 7 (first Xephyr run — bar/compositor fixes):** (1) `drw`: eliminate dedicated `cairo_xcb` connection — Cairo surface now uses `drw->xc` directly, eliminating two-connection race where `xcb_copy_area` could run before Cairo flushed text; `drw_map()` calls `cairo_surface_flush()` before `xcb_copy_area`; removes blocking round-trip syncs that starved the status timer. (2) `compositor`: fix `comp_repaint_idle()` discarding non-damage events — drain loop now dispatches all event types through `compositor_handle_event`; fix fullscreen unredirect condition (`!cw->redirected` not `cw->client->isfullscreen`). (3) `events`/`monitor`: fix `checkotherwm()` using uninitialised `root` — derives root from `xcb_get_setup` before `setup()` runs; add `compositor_handle_event` calls to `arrange()` and `restack()` drain loops |
| `bee5b95` | `compositor`: add `compositor_defer_fullscreen_bypass()` with 40 ms GLib timeout so clients process `ConfigureNotify` (TIOCSWINSZ) before bypass; `client`: `setfullscreen()` uses deferred bypass; `monitor`: fix `tile()` gap accumulation (advance by ideal slot height, not hint-snapped `HEIGHT(c)`); `restack()` calls `compositor_raise_overlay()` to keep overlay on top; `events`: `xcb_error_handler` suppresses benign `BadIDChoice` from stale XDamage/Present EID refs; `awm`: intern additional `_NET_WM_WINDOW_TYPE` atoms (Dock, Toolbar, Utility, Splash); fix `sni_init()` passing `cairo_xcb` conn instead of `xc` |
| `2598c82` | Full code audit — 18 bugs fixed: `util`: add `_Noreturn` to `die()`; `client`: fix `getwmicon` 64-bit `uint32_t` truncation, `resizeclient` used `selmon` instead of `c->mon`, `updatesizehints` aspect hint div-by-zero; `spawn`: fix UAF/heap overflow/`snprintf` size; `events`: `configurerequest` isfullscreen guard, `configurenotify` protect old\* for fullscreen; `monitor`: `createmon` NULL return checks, remove duplicate init loop; `systray`: `removesystrayicon` `if(*ii)` fix, `updatesystray` unsigned underflow; `drw`: `xfont_create` NULL cairo surface guard, `drw_text` unsigned underflow `w-lpad`, `drw_pic` UB on `w==0`/`h==0`; `ewmh`/`awm`: `setdesktopnames` use cached atom, `updateworkarea` write `4*TAGSLENGTH` tuples; `awm`: `cleanup()` free global `cl` |
| `6c0b442` | Fix wallpaper not displayed: `xcb_intern_atom` passed wrong string lengths for `_XROOTPMAP_ID` (12→13) and `ESETROOT_PMAP_ID` (15→16), so atoms never matched `feh`-set properties; handle `xcb_render_create_picture` failure in `comp_update_wallpaper` |
| `154c562` | Fix systray icons not docking: (1) `events.c` sent `XEMBED_EMBEDDED_NOTIFY` with `netatom[Xembed]` (`_NET_WM_NAME`) instead of `xatom[Xembed]` (`_XEMBED`); (2) `systray.c` overwrote initial event mask with a second `xcb_change_window_attributes` call — folded all flags into the `xcb_create_window` call |
| `6bcc1ba` | EGL/GL compositor correctness fixes: warn instead of silently return on `eglCreateImageKHR` failure; skip XRender picture for wallpaper in GL mode; check `GL_OES_EGL_image` GL extension after `eglMakeCurrent`; check `EGL_KHR_image_base` alongside `EGL_KHR_image_pixmap`; remove dead `u_flip_y` uniform (shader, struct field, all `glUniform1i` calls) |
| `25caf9e` | Compositor: replace blocking vsync with X Present vblank loop — `eglSwapInterval(0)`; `schedule_repaint()` arms `xcb_present_notify_msc` instead of blocking in `eglSwapBuffers`; add CPU-side dirty bbox in `CompShared`; split compositor into `compositor.c`, `compositor_egl.c`, `compositor_xrender.c`, `compositor_backend.h` |
| `13ab97e` | `awm`: add `-s` flag to skip autostart (useful for test runs; forwarded on `Mod+Shift+R` restart); fix `scan()` tiling XEMBED icon windows on restart by skipping windows with `_XEMBED_INFO` set |
| `d99912b` | `sni`: restore real icon rendering that was deleted during the XCB migration |
| `ad2f43d` | Compositor audit — 12 findings fixed: double-free in EGL/XRender cleanup, cancel bypass timeout in `compositor_cleanup`, early return on `NameWindowPixmap` failure, guard invalid XID on `CreatePicture` failure, remove tautological ternary in `xrender_init`, warn on defensive picture-free path, route `ShapeNotify` through vtable `apply_shape` slot, validate `xcb_create_pixmap` with checked variant, guard `compositor_damage_errors` on `comp.active`, flush before `xcb_request_check` in `comp_free_win`, remove dead `int i` |
| `5cf6bd0` | `launcher`: fix `xkb_keysym_to_utf8` NUL terminator off-by-one — subtract 1 from return value before passing to `launcher_insert_text` so embedded NULs do not break `strstr` filtering |
| `6a533bc` | Audit — 9 bugs fixed: `events.c` NULL systray guard, `xflush()` after `xcb_allow_events`, `numlockmask \|=` fix, `xflush()` outside `if(!sendevent)`; `systray.c` NULL guard; `client.c` ungrab before early return, `sendmon()` list detach/attach, operator precedence `(curtag-1)%LENGTH`, `int32_t` cast for off-screen hide; `awm.c` free systray icons list + colormap; `drw.c` remove dead cursor-context block, check `cairo_surface_status` after surface create |
| `d3c10da` | `drw`: add pure Cairo backend `src/drw_cairo.c` (selectable via `DRW_CAIRO=1`) — `drw_rect`/`drw_text`/`drw_pic` use only Cairo in the hot path; `drw_clr_create` does hex parse only (no `xcb_alloc_color` round-trip); `drw_map` remains `xcb_copy_area` |
| `2ec7cac` | `drw`: make `drw_cairo.c` the default backend; use `make DRW_LEGACY=1` to fall back to `drw.c` |
| `d791ad4` | `menu`/`launcher`: migrate from custom XCB override-redirect windows + `drw_*` rendering to native GTK widgets (`GtkMenu` / `GtkWindow` + `GtkSearchEntry` + `GtkListBox`); public API preserved, no callers changed |
| `68657d8` | `launcher`: connect `delete-event` to hide instead of destroy |
| `1f456cb` | `launcher`: set `override_redirect` on `GdkWindow` at realize time so awm does not manage the launcher window as a tiled client |
| `d9ffcc1` | `events`/`launcher`/`menu`: fix grab freeze (`xcb_allow_events(SYNC_POINTER)` before `sni_handle_click`), launcher `override_redirect` re-applied unconditionally in `launcher_show`, SNI menu popup uses synthetic `GdkEventButton` with real timestamp so GTK acquires a valid seat grab |
| `af0f487` | `sni`: for absolute `.svg`/`.SVG` icon paths, fall back to synchronous `icon_load_svg()` via librsvg/Cairo instead of the `gdk_pixbuf` async path (which requires an SVG loader that may be absent) |
| `b4094bd` | `awm`/`sni`/`menu`/`launcher`: IPC refactor — split launcher/SNI menus into `awm-ui` helper process over `SOCK_SEQPACKET` socketpair; awm forks selected commands itself via `UI_MSG_LAUNCHER_EXEC`; 16 code-review fixes (generation guards, `sni_menu_pending` reset, `deactivate_handler` disconnect, dead field removal, `dbus_retry_id` guard, `xsource_use_gtk_main_quit` rename, payload size reduction) |

---

## Post-migration work (Phase 7+)

After the XCB migration was declared complete, a series of runtime correctness
fixes and feature improvements were landed on this branch:

### Phase 7 — First Xephyr run fixes (`af0e300`, `bee5b95`, `2598c82`)

Three bugs found on first real run in Xephyr and fixed:

1. **`checkotherwm()` used uninitialised `root`** — `root` global set in
   `setup()` but `checkotherwm()` called before it; added early root derivation
   from `xcb_get_setup` in `main()`.
2. **Cairo rendered into wrong XCB connection** — `drw_create()` opened a
   separate connection for Cairo; `xcb_copy_area` could race with Cairo's
   internal queue. Fixed to share `drw->xc`; `drw_map()` calls
   `cairo_surface_flush()` before the copy.
3. **Present EIDs not proper XIDs** — `comp.present_eid_next` was a plain
   counter; `xcb_present_select_input` requires `xcb_generate_id()` XIDs.
   Fixed; `present_eid_next` field removed.

### Compositor refactor (`25caf9e`)

Split the monolithic `compositor.c` into:
- `src/compositor.c` — shared state, public API
- `src/compositor_egl.c` — EGL/GL backend
- `src/compositor_xrender.c` — XRender fallback backend
- `src/compositor_backend.h` — vtable + shared inline helpers

Replaced blocking vsync (`eglSwapInterval(1)`) with an X Present vblank
loop: `schedule_repaint()` arms `xcb_present_notify_msc`; vblank fires the
repaint and re-arms. CPU-side dirty bbox in `CompShared` eliminates a
synchronous `xcb_xfixes_fetch_region` round-trip before every swap.

### GTK migration for launcher and menu (`d791ad4`–`b4094bd`)

`launcher.c` and `menu.c` were rewritten from custom XCB override-redirect
windows with `drw_*` rendering to native GTK widgets (`GtkWindow` +
`GtkSearchEntry` + `GtkListBox`; `GtkMenu` + `popup_at_rect`).  Public API
preserved; no callers required changes.  GTK drives input through the
existing GLib main loop.

The `awm-ui` helper process (`src/awm_ui.c`) was added in `b4094bd` to host
GTK popup menus out-of-process via a `SOCK_SEQPACKET` socketpair, allowing
awm to fork commands itself rather than routing through the UI process.

### Pure-Cairo `drw` backend (`d3c10da`, `2ec7cac`)

`src/drw_cairo.c` is a drop-in replacement for `src/drw.c` with no XCB
drawing calls in the hot path.  It is now the default (`make`); use
`make DRW_LEGACY=1` to fall back to `drw.c`.

### Bug fix summary (post-migration)

| Commit | Description |
|--------|-------------|
| `6c0b442` | Wallpaper: off-by-one atom name lengths for `_XROOTPMAP_ID` / `ESETROOT_PMAP_ID` |
| `154c562` | Systray: wrong atom in `XEMBED_EMBEDDED_NOTIFY`; event mask overwrite in `xcb_create_window` |
| `6bcc1ba` | EGL: warn on `eglCreateImageKHR` failure; correct `GL_OES_EGL_image` + `EGL_KHR_image_base` checks |
| `13ab97e` | `-s` flag to skip autostart; `scan()` skips XEMBED icon windows on restart |
| `d99912b` | SNI: restore real icon rendering deleted during migration |
| `ad2f43d` | Compositor: 12 audit findings (double-free, bypass cancel, NULL guards, vtable routing) |
| `5cf6bd0` | Launcher: `xkb_keysym_to_utf8` NUL off-by-one broke multi-character filtering |
| `6a533bc` | 9 bugs: NULL systray guards, `numlockmask \|=`, `sendmon` list ops, operator precedence, colormap leak |
| `2598c82` | 18 bugs: `_Noreturn die()`, 64-bit icon, spawn UAF, fullscreen guards, gap accumulation, aspect div-by-zero |
| `bee5b95` | Deferred fullscreen bypass, tile gap fix, overlay z-order, `BadIDChoice` suppression |
| `d9ffcc1` | Grab freeze (`SYNC_POINTER`), override-redirect re-apply, GTK seat grab synthetic event |
| `af0f487` | SNI SVG icons via librsvg when gdk-pixbuf SVG loader absent |
| `b4094bd` | IPC refactor + 16 code-review fixes (generation guards, stale callback disconnect, dead fields) |

---

## New libraries added (`config.mk`)

```makefile
XCBLIBS = $(shell pkg-config --libs xcb-icccm xcb-randr xcb-keysyms) -lxkbcommon
XCBINC  = $(shell pkg-config --cflags xcb-icccm xcb-randr xcb-keysyms)
```

Both are added to `INCS` and `LIBS` respectively.  The linker flags in the build output
confirm these are being picked up:
`-lxcb-icccm -lxcb-randr -lxcb-keysyms`

Compositor extension libs (added directly to `COMPOSITORLIBS` in `config.mk`):
```
-lxcb-composite -lxcb-damage -lxcb-xfixes -lxcb-shape -lxcb-render-util
-lxcb-render -lxcb-present -l:libX11-xcb.so.1 -lxcb
```

New global in `src/awm.c` / declared `extern` in `src/awm.h`:
```c
xcb_key_symbols_t *keysyms;   // allocated in setup(), freed in cleanup()
```

New includes in `src/awm.h` (after `<X11/keysym.h>`):
```c
#include <xcb/randr.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
```

---

## API mapping cheat-sheet

### xcb-icccm

| Xlib | XCB |
|------|-----|
| `XGetWMHints` | `xcb_icccm_get_wm_hints` cookie + `xcb_icccm_get_wm_hints_reply` |
| `XSetWMHints` | `xcb_icccm_set_wm_hints` |
| `XGetWMNormalHints` | `xcb_icccm_get_wm_normal_hints` cookie + `xcb_icccm_get_wm_normal_hints_reply` |
| `XWMHints.flags` | `xcb_icccm_wm_hints_t.flags` (`int32_t`); flags are `XCB_ICCCM_WM_HINT_*` |
| `XWMHints.input` | `xcb_icccm_wm_hints_t.input` |
| `XSizeHints.flags` | `xcb_size_hints_t.flags` (`uint32_t`); flags are `XCB_ICCCM_SIZE_HINT_*` |
| `size.min_aspect.y/x` | `xcb_size_hints_t.min_aspect_den` / `.min_aspect_num` |
| `size.max_aspect.x/y` | `xcb_size_hints_t.max_aspect_num` / `.max_aspect_den` |

### xcb-keysyms

| Xlib | XCB |
|------|-----|
| `XkbKeycodeToKeysym(dpy, kc, 0, 0)` | `xcb_key_symbols_get_keysym(keysyms, kc, 0)` → `xcb_keysym_t` |
| `XRefreshKeyboardMapping(ev)` | populate `xcb_mapping_notify_event_t` from `XMappingEvent` fields; `xcb_refresh_keyboard_mapping(keysyms, &mne)` |
| `XGetModifierMapping` + `XKeysymToKeycode` + `XFreeModifiermap` | `xcb_get_modifier_mapping` + `xcb_key_symbols_get_keycode(keysyms, XK_Num_Lock)` (returns malloc'd `xcb_keycode_t*` array terminated by `XCB_NO_SYMBOL` — must `free()`) |

### xcb-randr

| Xlib | XCB |
|------|-----|
| `XRRQueryExtension` + `XRRSelectInput` | `xcb_get_extension_data(xc, &xcb_randr_id)` → check `ext->present`; `xcb_randr_select_input(xc, root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE)` |
| `XRRGetScreenResources` | `xcb_randr_get_screen_resources` cookie + reply |
| `XRRGetCrtcInfo` | `xcb_randr_get_crtc_info` cookie + reply |
| CRTC pointer | `xcb_randr_get_screen_resources_crtcs(sr)` — pointer into reply buffer, use **before** `free(sr)` |

---

## Intentional keeps — do NOT migrate these

All Xlib has been removed.  The following things have no XCB equivalent and are
implemented using inline protocol constants in `src/x11_constants.h` or are simply gone:

| What | How it was handled |
|------|--------------------|
| `XSetErrorHandler` / `xerror()` | Replaced by `xcb_error_handler()` in `events.c`, wired into `x_dispatch_cb()` |
| X11 error code names | Inline string table in `xcb_error_text()` (`events.c`) |
| `LASTEvent` | Defined in `src/x11_constants.h` as `36` |
| `KeySym` type | `typedef uint32_t KeySym` in `src/x11_constants.h` |
| `X_ConfigureWindow` / `X_GrabButton` etc. | Eight `X_` request opcodes in `src/x11_constants.h` — XCB has no names for these |
| `XESetWireToEvent` / `comp.gl_dpy` (EGL) | Removed — compositor is now pure XCB; `EGL_PLATFORM_XCB_EXT` used directly |

---

## Remaining migration candidates

**Phase 1 (easy wins) is complete as of `ebfa6b6`.**

**Phase 2 (event loop rewrite) is complete as of `beaeb24`.**

**Phase 3b (global `Display *dpy` → `xcb_connection_t *xc`) is complete as of `615729e`.**

**Phase 3c (drw rewrite to pure XCB; Xlib types removed from all headers) is complete as of `23cf5a7`.**

**Phase 4 (eliminate all remaining Xlib types) is complete as of `016c377`.**

**Phase 5 (eliminate all remaining Xlib symbolic constants, dead headers, Bool/True/False) is complete as of `875085b`.**

**Phase 6 (pure XCB error handler; drop all Xlib link deps) is complete as of `dfb7153`.**

**Phase 6b (replace `<X11/X.h>` and `<X11/Xproto.h>` with XCB names + `src/x11_constants.h`) is complete as of `7d8e76d`.**

No X11 headers remain anywhere in `src/`.  `<xcb/xproto.h>` is pulled in transitively
via `<xcb/xcb.h>` (XCB itself, not Xlib).  `src/x11_constants.h` covers the three things
XCB has no name for: `KeySym`, `LASTEvent`, and the `X_` request opcodes.

All Xlib event-dispatch APIs (`XMaskEvent`, `XNextEvent`, `XPending`, `XCheckTypedEvent`,
`XPutBackEvent`) have been replaced with XCB equivalents.  All `handler[]` callbacks are
now `void(*)(xcb_generic_event_t*)`.  The global `dpy` is gone; all TUs now reference
the global `xcb_connection_t *xc` declared in `awm.h`.

**The migration is complete.  There are no remaining candidates.**

---

## Constraints (always apply)

- `-std=c11 -pedantic -Werror` — zero warnings, no VLAs, no implicit function declarations
- `xcb_connection_t *xc` is the global connection, declared `extern` in `src/awm.h`
- All XCB reply memory freed with `free()` not `XFree()`
- `XCB_CONFIG_WINDOW_*` value arrays must be in ascending bit-position order
- `xcb_icccm_wm_hints_t.flags` is `int32_t`; `xcb_size_hints_t.flags` is `uint32_t`
- `xcb_key_symbols_get_keycode()` returns a malloc'd `xcb_keycode_t*` array terminated by
  `XCB_NO_SYMBOL` — must `free()` it
- `xcb_randr_get_screen_resources_crtcs(sr)` is a pointer into the reply buffer — use
  before `free(sr)`
- `config.h` must be the last `#include` in files that include it
- LSP errors about `compositor_set_hidden` undeclared are pre-existing false positives

---

## Pango migration (`src/drw.c` / `src/drw.h`)

**Done as of `e6d4f72`.**

The Xft text-rendering path in `drw.c` was replaced with PangoCairo.  Summary of what
changed:

- `config.mk`: `FREETYPELIBS` (`-lfontconfig -lXft`) removed; `PANGOLIBS`/`PANGOINC`
  added via `pkg-config pangocairo`.
- `config.h` / `config.def.h`: font strings converted from Xft/fontconfig pattern format
  (`"Family:size=N"`) to Pango description format (`"Family Size"`), e.g.
  `"BerkeleyMono Nerd Font 12"`.
- `src/drw.h`: `<X11/Xft/Xft.h>` removed; `<pango/pangocairo.h>` added.
  `Fnt` struct now holds `PangoFontDescription *desc` + `unsigned int h`.
  `Clr` typedef changed from `XftColor` to `struct { unsigned long pixel; unsigned short r, g, b, a; }`.
- `src/drw.c`: `xfont_create` uses `pango_font_description_from_string` + `PangoFontMetrics`
  for line height; `drw_text` replaced per-codepoint `XftCharExists`/`XftFontMatch` loop
  with a single `PangoLayout` render via `pango_cairo_show_layout`; `drw_font_getexts`
  deleted (was internal-only, now superseded by `pango_layout_get_pixel_size`).
- `src/systray.c`: `clr_to_argb()` updated from `clr->color.red/green/blue` (XftColor
  fields) to `clr->r/g/b` (new Clr struct fields).
- **Bug fix** (`drw_fontset_getwidth_clamp`): the `n` argument (pixel-width clamp, `unsigned
  int`) was incorrectly passed as the `invert` parameter to `drw_text`.  Fixed to pass `0`.
  The function has no callers and is dead code inherited from dwm.

---

## Migration complete

`make clean && make` produces **zero warnings, zero errors** on clang with
`-std=c11 -pedantic -Werror -Wall`.

Final state:

- Zero `#include <X11/...>` in `src/` — no Xlib headers anywhere.
- Zero Xlib API calls in `src/` — verified by audit.
- `libX11` is not in `awm`'s direct `NEEDED` list (`readelf -d awm`); it appears only
  transitively via GTK (pulled in by the SNI/D-Bus system tray feature).
- All event handlers are `void(*)(xcb_generic_event_t*)`.
- Global connection is `xcb_connection_t *xc`, declared `extern` in `src/awm.h`.
- Text rendering uses PangoCairo; font strings are Pango description format.
- Compositor is split into `compositor.c` / `compositor_egl.c` /
  `compositor_xrender.c` with a shared `compositor_backend.h` vtable.
- Default `drw` backend is `src/drw_cairo.c` (pure Cairo, no XCB drawing in
  hot path); `make DRW_LEGACY=1` selects the original `src/drw.c`.
- `awm-ui` helper process (`src/awm_ui.c`) hosts GTK popup menus out-of-process
  over a `SOCK_SEQPACKET` socketpair.
