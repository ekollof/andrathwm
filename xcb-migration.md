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

## Intentional Xlib keeps — do NOT migrate these

These are permanently Xlib and should not be touched:

| Location | Reason |
|----------|--------|
| `XSetErrorHandler` / `XSetIOErrorHandler` / `xerror*` | No XCB equivalent |
| `XESetWireToEvent` in `compositor.c` | Mesa DRI3 hook — must stay Xlib |
| `XOpenDisplay` / `XCloseDisplay` for `comp.gl_dpy` | EGL display — permanent keep |
| `XDefaultScreen(dpy)` in `awm.c` setup | Used once to index XCB screen iterator; stays until `XOpenDisplay` is removed |
| `DefaultScreen(dpy)` in `launcher.c` | Passed to `drw_create(dpy, scr, …)` — Phase 3 |
| `XSetErrorHandler` / `XGetErrorText` in `events.c` | Permanent keep |
| `compositor_fix_wire_to_event` keeps `XEvent*` signature | Calls `XESetWireToEvent` (Xlib API); call sites cast: `(XEvent *)(void *)ev` |

---

## Remaining migration candidates

**Phase 1 (easy wins) is complete as of `ebfa6b6`.**

**Phase 2 (event loop rewrite) is complete as of `beaeb24`.**

**Phase 3b (global `Display *dpy` → `xcb_connection_t *xc`) is complete as of `615729e`.**

All Xlib event-dispatch APIs (`XMaskEvent`, `XNextEvent`, `XPending`, `XCheckTypedEvent`,
`XPutBackEvent`) have been replaced with XCB equivalents.  All `handler[]` callbacks are
now `void(*)(xcb_generic_event_t*)`.  The global `dpy` is gone; all TUs now reference
the global `xcb_connection_t *xc` declared in `awm.h`.

### Phase 3 — Drawing primitives (lower priority)

`src/drw.c` drawing calls with no XCB equivalent — out of scope for this branch unless
the Pango migration (already designed below) is implemented first.

### Compositor permanent Xlib keeps (`compositor.c`)

| Site | Why kept |
|------|----------|
| `XESetWireToEvent` | Mesa DRI3 hook — must stay Xlib |
| `XOpenDisplay` / `XCloseDisplay` for `comp.gl_dpy` | EGL display — permanent keep |

---

## Constraints (always apply)

- `-std=c11 -pedantic -Werror` — zero warnings, no VLAs, no implicit function declarations
- `XGetXCBConnection(dpy)` is available in every file via `src/awm.h`
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

**Goal:** Replace the Xft text-rendering path in `drw.c` with PangoCairo.  The Cairo
surface (`drw->cairo_surface`) already exists and is used for icon rendering — Pango can
render directly onto it, eliminating `XftDraw`, `XftFont`, `XftColor`, and every
`XftFontMatch`/`XftTextExtentsUtf8`/`XftDrawStringUtf8` call.

Benefits:
- Font fallback becomes automatic (Pango handles Unicode coverage internally).
- Font description strings become `"Family Size"` (Pango format) instead of
  `"family:size=N"` (Xft/fontconfig pattern) — simpler and more familiar.
- The manual per-codepoint `XftCharExists` loop and the `nomatches` ring-buffer cache are
  completely eliminated.
- `FcPattern` / `FcCharSet` / `FcConfigSubstitute` calls disappear.
- `<X11/Xft/Xft.h>` is removed from the include tree.

### Build system changes (`config.mk`)

`pangocairo` is already available on this machine (version 1.57.0) and `cairo` is already
linked via `SNILIBS` (`gtk+-3.0` pulls it in).  Add `pangocairo` explicitly and remove
`FREETYPELIBS`:

```makefile
# Remove this line:
FREETYPELIBS = -lfontconfig -lXft

# Add this line:
PANGOLIBS = $(shell pkg-config --libs pangocairo)
PANGOINC  = $(shell pkg-config --cflags pangocairo)
```

In the `INCS` and `LIBS` lines, replace `${FREETYPEINC}` / `${FREETYPELIBS}` with
`${PANGOINC}` / `${PANGOLIBS}`:

```makefile
INCS = -I. -Isrc -Ithird_party -I${X11INC} ${PANGOINC} ${SNIINC} ${XCBINC}
LIBS = -L${X11LIB} -lX11 ${XINERAMALIBS} ${RANDRLIBS} ${XSSLIBS} ${PANGOLIBS} ${SNILIBS} ${COMPOSITORLIBS} ${XCBLIBS}
```

Note: `pkg-config pangocairo` already brings in `pango`, `cairo`, `glib-2.0`, and
`fontconfig` transitively, so nothing else needs to be added.

### `config.h` and `config.def.h` — font string format

Both files define the `fonts[]` array.  Change the font string from Xft/fontconfig
pattern format to Pango description format in **both files**:

```c
// Before (Xft format):
static const char *fonts[] = {
    "BerkeleyMono Nerd Font:size=12",
};

// After (Pango format):
static const char *fonts[] = {
    "BerkeleyMono Nerd Font 12",
};
```

Pango description format: `"Family Style Size"` — the size is the last token, style words
(Bold, Italic, etc.) go between family and size.  There is no `:size=` syntax.

The `fonts[]` array is passed to `drw_fontset_create()`.  With Pango the first entry is
the primary font description; additional entries are no longer needed for fallback (Pango
handles that automatically), but the array can be kept for forward compat.

### `src/drw.h` — struct and type changes

```c
// Remove:
#include <X11/Xft/Xft.h>

// Add:
#include <pango/pangocairo.h>
```

Replace the `Fnt` struct:

```c
// Before:
typedef struct Fnt {
    Display     *dpy;
    unsigned int h;
    XftFont     *xfont;
    FcPattern   *pattern;
    struct Fnt  *next;
} Fnt;

// After:
typedef struct Fnt {
    unsigned int          h;       // line height in pixels (ascent + descent)
    PangoFontDescription *desc;    // owned; freed in xfont_free()
    struct Fnt           *next;    // kept for API compat; only head is used
} Fnt;
```

Replace the `Clr` typedef.  `XftColor` was used because it carries both the X11 pixel
value (for `XSetForeground`) and the `XRenderColor` channels (for `clr_to_argb` in
`systray.c`).  The replacement must preserve both:

```c
// Before:
typedef XftColor Clr;

// After:
typedef struct {
    unsigned long  pixel;  // X11 pixel value — used by drw_rect via XSetForeground
    unsigned short r, g, b, a;  // 16-bit channels — used by clr_to_argb() in systray.c
} Clr;
```

In `drw_clr_create()` populate these by parsing the hex color string manually or via
`XAllocNamedColor` / `XParseColor` (both Xlib, no round-trip cost on allocation):

```c
void
drw_clr_create(Drw *drw, Clr *dest, const char *clrname)
{
    XColor xc;
    if (!XParseColor(drw->dpy, DefaultColormap(drw->dpy, drw->screen),
            clrname, &xc))
        die("error, cannot parse color '%s'", clrname);
    if (!XAllocColor(drw->dpy, DefaultColormap(drw->dpy, drw->screen), &xc))
        die("error, cannot allocate color '%s'", clrname);
    dest->pixel = xc.pixel;
    dest->r     = xc.red;
    dest->g     = xc.green;
    dest->b     = xc.blue;
    dest->a     = 0xffff;
}
```

The `Drw` struct does not need new fields — `drw->cairo_surface` is already the surface
Pango will render onto.

### `src/drw.c` — implementation changes

#### Remove all Xft headers / variables

Remove from `drw_text()`:
```c
// Remove these declarations:
XftDraw  *d = NULL;
XftResult result;
FcCharSet *fccharset;
FcPattern *fcpattern, *match;
// And the entire nomatches ring-buffer struct
```

Remove from `drw_font_getexts()`:
```c
XGlyphInfo ext;
XftTextExtentsUtf8(font->dpy, font->xfont, (XftChar8 *) text, len, &ext);
```

#### `xfont_create()` — replace with Pango

```c
static Fnt *
xfont_create(Drw *drw, const char *fontname)
{
    Fnt                  *font;
    PangoFontDescription *desc;
    PangoContext         *ctx;
    PangoFontMetrics     *metrics;

    desc = pango_font_description_from_string(fontname);
    if (!desc) {
        awm_error("cannot load font: '%s'", fontname);
        return NULL;
    }

    font       = ecalloc(1, sizeof(Fnt));
    font->desc = desc;

    /* Measure line height using a temporary PangoContext on the Cairo surface */
    {
        cairo_t *tmp_cr = cairo_create(drw->cairo_surface);
        ctx     = pango_cairo_create_context(tmp_cr);
        cairo_destroy(tmp_cr);
    }
    metrics = pango_context_get_metrics(ctx, desc, NULL);
    font->h = (pango_font_metrics_get_ascent(metrics) +
               pango_font_metrics_get_descent(metrics)) / PANGO_SCALE;
    pango_font_metrics_unref(metrics);
    g_object_unref(ctx);

    return font;
}
```

#### `xfont_free()` — replace with Pango

```c
static void
xfont_free(Fnt *font)
{
    if (!font)
        return;
    pango_font_description_free(font->desc);
    free(font);
}
```

#### `drw_text()` — replace rendering loop

The entire per-codepoint `XftCharExists` loop, `XftFontMatch` fallback, `nomatches`
cache, and `XftDrawStringUtf8` call are replaced with a single `PangoLayout` render:

```c
int
drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    unsigned int lpad, const char *text, int invert)
{
    int          render = x || y || w || h;
    PangoLayout *layout;
    cairo_t     *cr;
    int          tw, th;

    if (!drw || (render && (!drw->scheme || !w)) || !text || !drw->fonts)
        return 0;

    if (!render) {
        /* measurement-only mode */
        cr     = cairo_create(drw->cairo_surface);
        layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, drw->fonts->desc);
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_size(layout, &tw, NULL);
        g_object_unref(layout);
        cairo_destroy(cr);
        return tw;
    }

    /* Fill background */
    XSetForeground(drw->dpy, drw->gc,
        drw->scheme[invert ? ColFg : ColBg].pixel);
    XFillRectangle(drw->dpy, drw->drawable, drw->gc, x, y, w, h);
    if (drw->cairo_surface)
        cairo_surface_mark_dirty_rectangle(drw->cairo_surface, x, y, w, h);

    /* Render text via PangoCairo */
    cr     = cairo_create(drw->cairo_surface);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, drw->fonts->desc);
    pango_layout_set_text(layout, text, -1);

    /* Ellipsize if needed */
    pango_layout_set_width(layout, (int)(w - lpad) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    pango_layout_get_pixel_size(layout, &tw, &th);

    /* Set foreground colour */
    {
        Clr *fg = &drw->scheme[invert ? ColBg : ColFg];
        cairo_set_source_rgba(cr,
            fg->r / 65535.0, fg->g / 65535.0, fg->b / 65535.0,
            fg->a / 65535.0);
    }

    /* Vertically centre */
    cairo_move_to(cr, x + lpad, y + (int)(h - th) / 2);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_destroy(cr);

    return x + w;
}
```

#### `drw_font_getexts()` — remove entirely

`drw_font_getexts` is only ever called from within `drw.c` itself (confirmed by audit).
It is not part of the external API used by any other file.  After the migration:

- Delete the implementation from `drw.c`.
- Remove the declaration from `drw.h`.
- The `drw.c` call site disappears because the per-segment measurement loop is gone.

```c
// DELETE from drw.c and drw.h — no longer needed
void drw_font_getexts(Fnt *font, const char *text, unsigned int len,
    unsigned int *w, unsigned int *h);
```

### `src/systray.c` — `clr_to_argb()` update

`clr_to_argb` currently reads `clr->color.red`, `clr->color.green`, `clr->color.blue`
which are `XftColor` fields.  After the `Clr` typedef change those become `clr->r`,
`clr->g`, `clr->b`:

```c
// Before:
return 0xFF000000UL | ((unsigned long) (clr->color.red   >> 8) << 16) |
                      ((unsigned long) (clr->color.green  >> 8) <<  8) |
                      ((unsigned long) (clr->color.blue   >> 8));

// After:
return 0xFF000000UL | ((unsigned long) (clr->r >> 8) << 16) |
                      ((unsigned long) (clr->g >> 8) <<  8) |
                      ((unsigned long) (clr->b >> 8));
```

Same fix applies to the three `scheme[SchemeNorm][ColFg].color.{red,green,blue}` field
accesses at `systray.c:112-114`.

### `src/awm.h` — include cleanup

Remove:
```c
#include <X11/Xft/Xft.h>
```

This is currently at `awm.h:9`.  After the migration nothing outside `drw.c` references
Xft types directly (the `Clr` typedef will be the new struct, not `XftColor`).

### Invariants to preserve

| Invariant | Why |
|-----------|-----|
| `Clr.pixel` field must remain | `drw_rect()` passes it to `XSetForeground` (`drw.c:294-295`) |
| `Fnt.h` field must remain | Used as `bh` and `lrpad` throughout `awm.c`, `monitor.c`, `launcher.c`, `menu.c` |
| `drw_text()` return value semantics | Returns new x position after text; callers rely on this |
| `drw_fontset_getwidth()` unchanged | Calls `drw_text(drw, 0,0,0,0,0, text, 0)` — measurement mode must still return pixel width |
| `drw_fontset_getwidth_clamp()` unchanged | Same |
| `drw->cairo_surface` must be valid before `xfont_create` is called | `xfont_create` needs a `cairo_t` to build a `PangoContext` for metrics |
| `drw_fontset_create` called after `drw_create` | Already the case in `awm.c:582` |
| Font fallback | Handled automatically by Pango — no code needed |
| `drw->fonts->next` chain | Can be preserved structurally for API compat, but Pango ignores it; the primary font's `PangoFontDescription` drives everything including fallback |

### Files that do NOT need changes

- `src/monitor.c` — uses `drw_text`, `drw_rect`, `drw_map`, `drw_setscheme` — all
  unchanged externally
- `src/launcher.c` — same
- `src/menu.c` — same
- `src/awm.c` — calls `drw_fontset_create`, `drw_scm_create`, `drw_free` — all unchanged
  externally
- `src/xrdb.c` — calls `drw_scm_create` with hex color strings — compatible with new
  `drw_clr_create`
- `src/compositor.c` — no drw usage at all

---

## What "done" looks like

The migration is complete when:

1. ~~**Phase 2** (event loop rewrite) is implemented~~ — **Done as of `beaeb24`.**
2. ~~**Phase 3b** (global `Display *dpy` → `xcb_connection_t *xc`) is implemented~~ — **Done as of `615729e`.**
3. **Phase 3c / Pango migration** is implemented: `drw.c` / `drw.h` rewritten,
   `config.mk` updated, `config.h` + `config.def.h` font strings updated, `systray.c`
   `Clr` field accesses updated, `awm.h` Xft include removed, `drw_font_getexts` deleted.
4. `make clean && make` produces zero warnings and zero errors.
5. The branch is rebased cleanly onto `main` (or merged) and pushed.

At that point:
- `XGetWindowProperty` will be fully eliminated from the core WM files.
- `<X11/Xft/Xft.h>` will be gone from the build.
- Text rendering will go through PangoCairo onto the existing `cairo_xcb_surface`.
- The only remaining Xlib in `drw.c` will be `XCreatePixmap`, `XCreateGC`,
  `XSetLineAttributes`, `XFreePixmap`, `XFreeGC`, `XCopyArea`, `XSetForeground`,
  `XFillRectangle`, `XDrawRectangle`, `XCreateFontCursor`, `XFreeCursor` — drawing
  primitives without practical XCB equivalents, out of scope for this branch.
