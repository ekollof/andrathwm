# Multi-Monitor Support

awm supports both RandR and Xinerama for multi-monitor setups.

## RandR (Recommended)

RandR (Resize and Rotate) is the modern X11 extension for multi-monitor support.

### Features
- **Dynamic hotplug** - Add/remove monitors without restarting awm
- **Automatic reconfiguration** - Responds to monitor changes via xrandr
- **Better compatibility** - Works with modern drivers and compositors
- **Per-output control** - Supports rotation, scaling, etc. (via xrandr)

### How It Works
- awm detects RandR 1.2+ on startup
- Monitors RRScreenChangeNotify events
- Automatically updates monitor layout when configuration changes
- Falls back to Xinerama if RandR unavailable

### Usage
```bash
# Add a monitor
xrandr --output HDMI-1 --auto --right-of eDP-1

# Remove a monitor  
xrandr --output HDMI-1 --off

# awm automatically detects and adapts to changes
```

## Xinerama (Fallback)

Xinerama is the legacy multi-monitor extension.

### Features
- **Universal compatibility** - Works everywhere
- **Stable and tested** - Mature codebase
- **Simple** - Easy to understand and debug

### Limitations
- **No hotplug** - Must restart awm to add/remove monitors
- **Static only** - Cannot detect runtime configuration changes
- **Legacy** - No longer actively developed

### When Used
- RandR not available on the X server
- RandR extension check fails
- Older systems without RandR 1.2+

## Priority Order

awm tries multi-monitor support in this order:

1. **RandR** - If available, use RandR for dynamic support
2. **Xinerama** - Fallback if RandR unavailable
3. **Single monitor** - Default if neither available

## Building

Both extensions are enabled by default in `config.mk`:

```make
# RandR (modern multi-monitor support)
RANDRLIBS  = -lXrandr
RANDRFLAGS = -DXRANDR

# Xinerama (legacy fallback)
XINERAMALIBS  = -lXinerama
XINERAMAFLAGS = -DXINERAMA
```

To disable either:
- Comment out RANDRFLAGS to disable RandR
- Comment out XINERAMAFLAGS to disable Xinerama

## Verification

Check which mode awm is using:

```bash
# Check if RandR extension is available
xrandr --version

# Check if Xinerama is active
xdpyinfo | grep -i xinerama

# Monitor awm behavior
# RandR: hotplug works, monitors auto-update
# Xinerama: need to restart awm for monitor changes
```

## Troubleshooting

### Hotplug not working
- Verify RandR extension: `xrandr --version`
- Check if awm was compiled with `-DXRANDR`
- awm may have fallen back to Xinerama

### Monitors not detected
- Check `xrandr` output shows all monitors
- Verify monitors are enabled (not `--off`)
- Check for duplicate geometries (clone mode)

### Wrong monitor order
- Use `xrandr --output <name> --primary` to set primary
- Arrange with `--left-of`, `--right-of`, etc.
- awm respects the order from xrandr/Xinerama

## Advanced

### Primary Monitor
RandR supports a primary monitor concept. Set with:
```bash
xrandr --output eDP-1 --primary
```

awm will prefer placing new windows on the primary monitor.

### Clone/Mirror Mode
If monitors have identical geometries, awm treats them as one monitor
to avoid confusion. This applies to both RandR and Xinerama.

### Per-Monitor Bar
awm creates a separate bar for each detected monitor. The bar position
is automatically calculated based on monitor geometry.
