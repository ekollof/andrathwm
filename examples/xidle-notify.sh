#!/bin/sh
# xidle-notify.sh - Send notifications based on idle time
#
# Usage: xidle-notify.sh [warn_seconds] [lock_seconds]
#
# Example:
#   xidle-notify.sh 240 300  # Warn at 4min, notify lock at 5min
#
# To run at startup, add to your .xinitrc or dwm autostart:
#   xidle-notify.sh 240 300 &
#
# Note: Requires notify-send (from libnotify)

# Default timeouts (in seconds)
WARN_TIME="${1:-240}"   # 4 minutes - warn user
LOCK_TIME="${2:-300}"   # 5 minutes - notify about impending lock

# Convert to milliseconds
WARN_MS=$((WARN_TIME * 1000))
LOCK_MS=$((LOCK_TIME * 1000))

# Track if we've already sent notifications
WARNED=0
LOCK_WARNED=0

echo "xidle-notify: Will warn at ${WARN_TIME}s, lock notice at ${LOCK_TIME}s"

# Main loop
while true; do
	# Get current idle time in milliseconds
	IDLE=$(xidle)
	
	# Reset warning flags when user becomes active again
	if [ "$IDLE" -lt "$WARN_MS" ]; then
		WARNED=0
		LOCK_WARNED=0
	fi
	
	# Check for lock warning
	if [ "$IDLE" -ge "$LOCK_MS" ] && [ "$LOCK_WARNED" -eq 0 ]; then
		SECONDS_IDLE=$((IDLE / 1000))
		notify-send -u critical -t 5000 \
			"Screen Lock" \
			"Idle for ${SECONDS_IDLE}s - Screen will lock soon"
		LOCK_WARNED=1
	# Check for initial warning
	elif [ "$IDLE" -ge "$WARN_MS" ] && [ "$WARNED" -eq 0 ]; then
		SECONDS_IDLE=$((IDLE / 1000))
		notify-send -u normal -t 3000 \
			"Idle Warning" \
			"You've been idle for ${SECONDS_IDLE} seconds"
		WARNED=1
	fi
	
	# Check every 5 seconds
	sleep 5
done
