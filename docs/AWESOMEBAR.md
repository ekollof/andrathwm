# Awesomebar Implementation

This awm build includes the awesomebar patch with window icons support.

## Features

### Visual
- All windows on current tag displayed as tabs in the title bar
- Window icons (16x16) from _NET_WM_ICON property
- Focused window highlighted with selection colors
- Hidden windows shown with normal colors
- Empty rectangle indicator for hidden windows
- Floating window indicator preserved

### Mouse Actions
- **Left-click unfocused window** → Focus that window
- **Left-click focused window** → Hide (minimize) that window
- **Left-click hidden window** → Restore and focus that window
- **Middle-click window** → Zoom window

### Keyboard Shortcuts

| Keybinding | Action |
|------------|--------|
| `Mod+j` / `Mod+k` | Cycle through visible windows only |
| `Mod+Shift+j` / `Mod+Shift+k` | Cycle through ALL windows (hidden windows temporarily shown) |
| `Mod+h` | Hide current window |
| `Mod+s` | Restore most recently hidden window |
| `Mod+Shift+s` | Show all hidden windows in current workspace |

### Modified Keybindings

Due to conflicts with awesomebar bindings, these were changed:

| Old Binding | New Binding | Action |
|-------------|-------------|--------|
| `Mod+h` / `Mod+l` | `Mod+Control+h` / `Mod+Control+l` | Resize master area |
| `Mod+Shift+j` / `Mod+Shift+k` | `Mod+Control+j` / `Mod+Control+k` | Move window in stack |

## Implementation Details

- **Icon Loading**: Icons loaded from `_NET_WM_ICON` X11 property
- **Icon Cache**: Icons cached per-window for performance
- **Icon Fallback**: Missing icons show no icon (space reserved)
- **Window State**: Hidden state preserved using X11 IconicState
- **Bar Layout**: Equal-width tabs distributed across available space

## Compatibility

Works alongside:
- StatusNotifier (SNI) systray icons
- XEMBED systray support
- All existing awm patches
