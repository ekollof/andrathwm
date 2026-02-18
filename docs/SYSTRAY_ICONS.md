# Systray Icon Configuration

## Getting Symbolic/Monochrome Icons

awm now sets the `_NET_SYSTEM_TRAY_COLORS` property to hint systray applications
about preferred icon colors. However, to get truly minimal or symbolic icons,
you need to configure the applications themselves.

### For GTK Applications

Create or edit `~/.config/gtk-3.0/settings.ini`:

```ini
[Settings]
gtk-icon-theme-name=Adwaita
gtk-application-prefer-dark-theme=1
```

Or set environment variables in your `.xinitrc` or session startup:

```bash
export GTK_THEME=Adwaita:dark
```

### For Qt Applications

Create or edit `~/.config/qt5ct/qt5ct.conf`:

```ini
[Appearance]
icon_theme=breeze-dark
```

Or use `qt5ct` GUI to select an icon theme with symbolic icons.

### Icon Themes with Good Symbolic Icons

- **Adwaita** - GNOME's default, excellent symbolic icons
- **Papirus** - Has both colorful and symbolic variants
- **Breeze** - KDE's default, good symbolic support
- **elementary** - Clean symbolic icons

### Visual Depth

awm now sets `_NET_SYSTEM_TRAY_VISUAL` to prefer 32-bit ARGB visual when
available, allowing icons with transparency to render properly.

### After Changes

Remember to restart both awm and your systray applications for icon theme
changes to take effect.
