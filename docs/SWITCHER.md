# Alt+Tab / Super+Tab Window Switcher

awm includes a built-in window switcher that shows a horizontal row of window
preview cards when Alt+Tab or Super+Tab is pressed.

## Keybindings

| Binding | Action |
|---|---|
| `Alt+Tab` | Open switcher (visible windows), advance forward |
| `Alt+Shift+Tab` | Open switcher (visible windows), advance backward |
| `Super+Tab` | Open switcher (all windows on all tags), advance forward |
| `Super+Shift+Tab` | Open switcher (all windows on all tags), advance backward |
| `Tab` / `Shift+Tab` | Cycle forward / backward while switcher is open |
| Release `Alt` or `Super` | Confirm selection and focus the chosen window |
| `Escape` | Cancel — return focus to the previously focused window |
| `Enter` | Confirm selection immediately |

## Window scope

- **Alt+Tab** — shows windows that are currently visible on any monitor's
  active tagset (`ISVISIBLE` across all monitors).
- **Super+Tab** — shows all managed windows regardless of tag or monitor.
  Selecting a window on a hidden tag calls `view()` to make that tag visible
  before focusing.

## Card layout

Each card in the switcher row contains:

- **Thumbnail** — a live-updating preview of the window's current content,
  scaled to at most 200×150 px while preserving aspect ratio.
- **Icon** — the application's window icon (24×24 px), from `_NET_WM_ICON`.
- **Title** — the window title, truncated with an ellipsis if needed.

The selected card is highlighted with the `SchemeSel` border colour. The
switcher window is centred on the focused monitor.

## Thumbnail rendering

Thumbnails are captured directly from the compositor and updated every 100 ms
while the switcher is open.

### EGL/GL backend (default)

The window's live `GL_TEXTURE_2D` (`cw->texture`) is rendered into a
framebuffer object at thumbnail size using the compositor's existing shader and
VAO. Pixels are read back via `glReadPixels` into a `cairo_image_surface`.
This always reflects the current frame — there is no pixmap staleness.

### XRender backend (fallback)

The window's `XRenderPicture` (`cw->picture`, wrapping the redirected pixmap)
is composited into a temporary destination pixmap at thumbnail size using a
projective scale transform. The destination pixmap is then read back with
`xcb_get_image` into a `cairo_image_surface`. The server-side picture is
always live, so this also reflects current content.

Both paths return a `cairo_image_surface` — no `cairo_surface_mark_dirty` is
needed.

## Architecture

The switcher is implemented entirely inside the `awm` process:

- **`src/switcher.c`** — GTK floating window, card widgets, thumbnail capture
  and refresh timer.
- **`src/compositor.c`** — `comp_capture_thumb(Client*, max_w, max_h)` public
  entry point; dispatches to the active backend.
- **`src/compositor_egl.c`** — `egl_capture_thumb` (GL FBO path).
- **`src/compositor_xrender.c`** — `xrender_capture_thumb` (XRender + get_image path).
- **`src/events.c`** — `keypress()` intercepts Tab/Escape/Enter while switcher
  is open; `keyrelease()` confirms on modifier release.

The switcher window uses `override_redirect` so awm does not manage it, and is
raised via `xcb_configure_window(XCB_STACK_MODE_ABOVE)` on every show. An
active `xcb_grab_keyboard` on the root window ensures all key events are
delivered to awm's own event loop while the switcher is visible.

## Configuration

The switcher keybindings are defined in `config.h` alongside other keybindings.
Visual tunables (card size, thumbnail size, padding, refresh interval) are
compile-time constants at the top of `src/switcher.c`:

```c
#define SW_MAX_THUMB_W  200   /* maximum thumbnail width  (px) */
#define SW_MAX_THUMB_H  150   /* maximum thumbnail height (px) */
#define SW_MIN_CARD_W   120   /* minimum card width       (px) */
#define SW_CARD_PAD       8   /* padding inside each card      */
#define SW_CARD_GAP       6   /* gap between cards             */
#define SW_ICON_SIZE     24   /* icon size in the title row    */
#define SW_TITLE_H       36   /* height of the title row       */
#define SW_BORDER_W       3   /* selection highlight thickness */
#define SW_WIN_PAD       12   /* padding around the card row   */
#define SW_REFRESH_MS   100   /* thumbnail refresh interval    */
```

## Requirements

The switcher requires the compositor to be active (`-DCOMPOSITOR` build flag,
which is enabled by default). If the compositor is not active, the switcher
still works but shows placeholder boxes instead of thumbnails.
