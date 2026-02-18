# xidle - X11 Idle Time Query Utility

`xidle` is a simple, lightweight utility for querying X11 idle time using the XScreenSaver extension. It's designed to work with any window manager and requires no systemd dependencies.

## Features

- Query user idle time (time since last keyboard/mouse activity)
- Two output formats: milliseconds or human-readable
- POSIX-compliant, works on Linux and BSD
- No dependencies beyond X11 and XScreenSaver extension
- Can be used in shell scripts for automation

## Usage

```sh
# Print idle time in milliseconds
xidle

# Print idle time in human-readable format (e.g., "5m 23s")
xidle -h
```

## Example Scripts

Four example scripts are located in the `examples/` directory and demonstrate common use cases:

### 1. Auto-lock (`examples/xidle-autolock.sh`)

Automatically lock the screen after a period of inactivity.

```sh
# Lock with slock after 5 minutes of inactivity
examples/xidle-autolock.sh 300 slock

# Lock with i3lock after 10 minutes
examples/xidle-autolock.sh 600 i3lock -c 000000

# Add to .xinitrc or awm autostart
examples/xidle-autolock.sh 300 slock &
```

### 2. DPMS Control (`examples/xidle-dpms.sh`)

Automatically manage display power based on idle time.

```sh
# Standby at 5min, suspend at 10min, off at 15min
examples/xidle-dpms.sh 300 600 900

# Standby at 10min, skip suspend, off at 20min
examples/xidle-dpms.sh 600 0 1200

# Add to .xinitrc or awm autostart
examples/xidle-dpms.sh 300 600 900 &
```

### 3. Idle Notifications (`examples/xidle-notify.sh`)

Send desktop notifications when user has been idle.

```sh
# Warn at 4 minutes, lock notice at 5 minutes
examples/xidle-notify.sh 240 300

# Add to .xinitrc or awm autostart
examples/xidle-notify.sh 240 300 &
```

### 4. Combined Manager (`examples/xidle-manager.sh`)

All-in-one script for lock and DPMS management.

```sh
# Lock at 5min, DPMS off at 10min
examples/xidle-manager.sh 300 600 slock

# Add to .xinitrc or awm autostart
examples/xidle-manager.sh 300 600 slock &
```

## Integration with awm

The `getidletime()` function is available in awm.c for internal use. You can use it in custom patches or modifications:

```c
unsigned long idle_ms = getidletime();
if (idle_ms > 300000) {  // 5 minutes
    // Do something when idle
}
```

## Building

xidle is built automatically when you build awm:

```sh
make clean && make
```

To build xidle standalone:

```sh
gcc -o xidle xidle.c -lX11 -lXss
```

## Installation

```sh
# Install both awm and xidle
sudo make install

# This installs xidle to /usr/bin
```

## Disabling XScreenSaver Support

If you don't want XScreenSaver extension support, edit `config.mk` and comment out:

```makefile
# XSSLIBS  = -lXss
# XSSFLAGS = -DXSS
```

## Use Cases

- **Auto-locking**: Lock screen after inactivity
- **DPMS control**: Manage display power states
- **Notifications**: Warn users about idle time
- **Screen recording**: Pause recording during idle periods
- **Time tracking**: Log active vs idle time
- **Presence indicators**: Update status based on activity
- **Resource management**: Pause background tasks when idle

## Advantages Over Other Solutions

- **No systemd dependency**: Works on any POSIX system
- **Minimal**: Single-purpose, does one thing well
- **Scriptable**: Easy to integrate with shell scripts
- **Fast**: Direct X11 query, no polling overhead
- **Portable**: Works with any window manager

## Comparison with Other Tools

| Tool | Idle Detection | Systemd Required | Multi-WM Support |
|------|----------------|------------------|------------------|
| xidle | XScreenSaver | No | Yes |
| xprintidle | XScreenSaver | No | Yes |
| xss-lock | XScreenSaver events | Optional | Yes |
| swayidle | Wayland idle | No | Wayland only |
| xautolock | X11 activity | No | Yes |

## Tips

1. **Combine scripts**: Run multiple scripts together for comprehensive idle management
   ```sh
   examples/xidle-dpms.sh 300 600 900 &
   examples/xidle-autolock.sh 600 slock &
   ```

2. **Test interactively**: Check idle time manually
   ```sh
   watch -n 1 'xidle -h'
   ```

3. **Prevent false locks**: Set appropriate timeouts for your workflow

4. **Use with cron**: Log idle patterns
   ```sh
   * * * * * xidle >> /tmp/idle-log.txt
   ```

## Troubleshooting

**xidle returns 0 constantly**
- XScreenSaver extension not available
- Check: `xset q | grep -A 2 "Screen Saver"`
- Enable: `xset s on`

**Scripts don't work**
- Ensure xidle is in your PATH
- Check script has execute permissions: `chmod +x examples/xidle-*.sh`
- Verify dependencies (xset, notify-send, etc.)

## License

See LICENSE file - same as AndrathWM (MIT/X Consortium License)

## Author

Part of the AndrathWM (awm) distribution
