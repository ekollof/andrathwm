#!/bin/sh
# xidle-manager.sh - Comprehensive idle management (lock + DPMS)
#
# Usage: xidle-manager.sh [lock_seconds] [dpms_seconds] [locker_command]
#
# Example:
#   xidle-manager.sh 300 600 slock  # Lock at 5min, DPMS off at 10min
#
# This combines auto-locking with DPMS control in a single script.
# The display will turn off at dpms_seconds, and the screen will lock
# at lock_seconds.
#
# To run at startup, add to your .xinitrc or dwm autostart:
#   xidle-manager.sh 300 600 slock &

# Default timeouts
LOCK_TIMEOUT="${1:-300}"    # 5 minutes
DPMS_TIMEOUT="${2:-600}"    # 10 minutes
LOCKER="${3:-slock}"

# Convert to milliseconds
LOCK_MS=$((LOCK_TIMEOUT * 1000))
DPMS_MS=$((DPMS_TIMEOUT * 1000))

# Track state
LOCKED=0
DISPLAY_OFF=0

echo "xidle-manager: Lock=${LOCK_TIMEOUT}s, DPMS=${DPMS_TIMEOUT}s, Locker='$LOCKER'"

# Enable DPMS
xset +dpms

# Main loop
while true; do
	# Get current idle time in milliseconds
	IDLE=$(xidle)
	
	# Reset state when user becomes active
	if [ "$IDLE" -lt "$LOCK_MS" ]; then
		LOCKED=0
		DISPLAY_OFF=0
		xset dpms force on
	fi
	
	# Turn off display if idle long enough
	if [ "$IDLE" -ge "$DPMS_MS" ] && [ "$DISPLAY_OFF" -eq 0 ]; then
		echo "xidle-manager: Turning off display after ${DPMS_TIMEOUT}s idle"
		xset dpms force off
		DISPLAY_OFF=1
	fi
	
	# Lock screen if idle long enough
	if [ "$IDLE" -ge "$LOCK_MS" ] && [ "$LOCKED" -eq 0 ]; then
		echo "xidle-manager: Locking screen after ${LOCK_TIMEOUT}s idle"
		$LOCKER
		LOCKED=1
		
		# After unlock, reset and wait before checking again
		sleep 2
		LOCKED=0
		DISPLAY_OFF=0
	fi
	
	# Check every 5 seconds
	sleep 5
done
