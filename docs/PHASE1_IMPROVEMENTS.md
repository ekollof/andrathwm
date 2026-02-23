# Historical: Phase 1 Improvements

> **Note:** This document describes early stability work done before the XCB
> migration. It is kept for historical reference only. The issues described
> here have all been resolved; several of the specific fixes were superseded
> by later rewrites (e.g. the Xlib thread-safety concern was eliminated
> entirely when Xlib was removed from the codebase).

---

**Date:** February 17, 2026
**Status:** Complete (superseded)

## Summary

Phase 1 focused on critical stability and security fixes to the initial awm
codebase:

1. Added `.gitignore`
2. Added NULL checks after `malloc`/`strdup` in `dbus.c`, `sni.c`, `icon.c`
3. Fixed memory leak in systray icon removal (`removesystrayicon`)
4. Fixed thread-safety violation in SNI icon rendering (Xlib calls from async
   D-Bus callbacks) — later made moot by the full removal of Xlib

These fixes landed before the XCB migration. See `xcb-migration.md` for the
full history of subsequent changes.
