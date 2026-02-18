#!/bin/sh
# xidle-dpms.sh - Automatic DPMS (display power management) control
#
# Usage: xidle-dpms.sh [standby_sec] [suspend_sec] [off_sec]
#
# Example:
#   xidle-dpms.sh 300 600 900  # Standby at 5min, suspend at 10min, off at 15min
#   xidle-dpms.sh 600 0 1200   # Standby at 10min, skip suspend, off at 20min
#
# To run at startup, add to your .xinitrc or dwm autostart:
#   xidle-dpms.sh 300 600 900 &
#
# Note: Requires xset for DPMS control

# Default timeouts (in seconds)
STANDBY="${1:-300}"   # 5 minutes
SUSPEND="${2:-600}"   # 10 minutes
OFF="${3:-900}"       # 15 minutes

# Convert to milliseconds
STANDBY_MS=$((STANDBY * 1000))
SUSPEND_MS=$((SUSPEND * 1000))
OFF_MS=$((OFF * 1000))

# Track current DPMS state
CURRENT_STATE="on"

echo "xidle-dpms: Standby=${STANDBY}s, Suspend=${SUSPEND}s, Off=${OFF}s"

# Enable DPMS
xset +dpms

# Main loop
while true; do
	# Get current idle time in milliseconds
	IDLE=$(xidle)
	
	# Determine what state we should be in
	if [ "$OFF" -gt 0 ] && [ "$IDLE" -ge "$OFF_MS" ]; then
		NEW_STATE="off"
	elif [ "$SUSPEND" -gt 0 ] && [ "$IDLE" -ge "$SUSPEND_MS" ]; then
		NEW_STATE="suspend"
	elif [ "$STANDBY" -gt 0 ] && [ "$IDLE" -ge "$STANDBY_MS" ]; then
		NEW_STATE="standby"
	else
		NEW_STATE="on"
	fi
	
	# Only change state if needed
	if [ "$NEW_STATE" != "$CURRENT_STATE" ]; then
		case "$NEW_STATE" in
			standby)
				echo "xidle-dpms: Entering standby mode"
				xset dpms force standby
				;;
			suspend)
				echo "xidle-dpms: Entering suspend mode"
				xset dpms force suspend
				;;
			off)
				echo "xidle-dpms: Turning display off"
				xset dpms force off
				;;
			on)
				echo "xidle-dpms: Waking display"
				xset dpms force on
				;;
		esac
		CURRENT_STATE="$NEW_STATE"
	fi
	
	# Check every 5 seconds
	sleep 5
done
