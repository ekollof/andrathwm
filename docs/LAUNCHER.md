# Application Launcher

awm includes a built-in rofi-style application launcher that replaces dmenu for
launching applications.

## Activation

| Keybinding | Action |
|------------|--------|
| `Mod4+p` | Open launcher |
| `Escape` | Close launcher |

The launcher opens centered on the focused monitor.

## Usage

Type to filter the list. Results are matched against application names
case-insensitively as you type.

### Keyboard Navigation

| Key | Action |
|-----|--------|
| `Up` / `Down` | Move selection |
| `Page Up` / `Page Down` | Scroll by page |
| `Home` / `End` | Jump to first / last item |
| `Return` | Launch selected application |
| `Escape` | Close without launching |

### Mouse Navigation

| Action | Effect |
|--------|--------|
| Click item | Select and launch |
| Scroll wheel | Move selection up/down |
| Click input area | Dismiss launcher |

## Sort Order

The launcher sorts results using launch history:

- The **top 10 most-launched** applications float to the top, sorted by launch
  count descending.
- Everything else (including items ranked 11th and beyond in history) is sorted
  **alphabetically by name**, case-insensitive.

This means that after a few days of use, your most-reached-for apps appear
immediately without any typing.

### Persistence

Launch counts are written to disk on every successful launch:

```
$XDG_STATE_HOME/awm/launcher_history
```

If `XDG_STATE_HOME` is not set, the path falls back to:

```
~/.local/state/awm/launcher_history
```

The file is plain text, one entry per line:

```
name<TAB>count
```

It is safe to delete; the launcher will recreate it from scratch. The directory
is created automatically on first launch.

## Application Sources

The launcher searches two sources, in this order:

1. **Installed `.desktop` files** — scanned from standard XDG directories:
   - `/usr/share/applications`
   - `/usr/local/share/applications`
   - `~/.local/share/applications`

   Only entries without `NoDisplay=true` are shown. Field codes (`%f`, `%u`,
   etc.) are stripped from `Exec` values before execution.

2. **PATH executables** — all executables found on `$PATH` that are not already
   represented by a `.desktop` entry.

## Icons

Icons are loaded from the GTK icon theme at 20×20 pixels. The lookup order is:

1. Direct theme lookup by the `Icon=` value from the `.desktop` file
2. Reverse-DNS alias resolution — e.g. an `Icon=Alacritty` entry is resolved to
   `com.alacritty.Alacritty` in the theme, if present
3. Absolute path — if the `Icon=` value is an absolute path, it is loaded
   directly

SVG icons are rasterised via librsvg so transparency is preserved correctly.
Applications without a resolvable icon show their name without an icon.

## Scrollbar

A scrollbar is displayed on the right edge when the filtered list exceeds 12
visible items. The thumb position reflects the current scroll position relative
to the full list.

## Configuration

The launcher is compiled in unconditionally. Behaviour constants are defined
at the top of `src/launcher.c`:

| Constant | Default | Description |
|----------|---------|-------------|
| `LAUNCHER_INPUT_HEIGHT` | 28 | Height of the search input box (px) |
| `LAUNCHER_ITEM_HEIGHT` | 24 | Height of each result row (px) |
| `LAUNCHER_MAX_VISIBLE` | 12 | Maximum rows shown before scrolling |
| `LAUNCHER_ICON_SIZE` | 20 | Icon size in pixels |
| `LAUNCHER_PADDING` | 8 | Internal padding (px) |
| `LAUNCHER_SCROLL_BAR_WIDTH` | 6 | Scrollbar width (px) |
| `LAUNCHER_HISTORY_TOP` | 10 | Number of most-used apps that float above alphabetic order |

To change these, edit `src/launcher.c` and recompile.

The keybinding is set in `config.h`:

```c
{ MODKEY, XK_p, launchermenu, {0} },
```

## Implementation Notes

- Icons are loaded once at startup (during `.desktop` file parsing) and cached
  as `cairo_surface_t` objects. There is no runtime re-scan.
- The reverse-DNS alias table is built lazily on first alias lookup and freed
  when the launcher is destroyed.
- The launcher reuses the same `Drw` instance as the bar, sharing font and
  colour scheme settings.
- Icon background rectangles are filled with `ColBg` before compositing so that
  semi-transparent icon edges blend against the correct row background colour
  rather than an X11 pixmap artefact.
</content>
</invoke>