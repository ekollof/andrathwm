#!/bin/sh
# xidle-autolock.sh - Auto-lock screen after idle timeout
#
# Usage: xidle-autolock.sh [timeout_seconds] [locker_command]
#
# Example:
#   xidle-autolock.sh 300 slock        # Lock with slock after 5 minutes
#   xidle-autolock.sh 600 i3lock -c 000000  # Lock with i3lock after 10 minutes
#
# To run at startup, add to your .xinitrc or dwm autostart:
#   xidle-autolock.sh 300 slock &

# Default timeout: 5 minutes (300 seconds)
TIMEOUT="${1:-300}"
shift

# Default locker: slock
LOCKER="${*:-slock}"

# Convert timeout to milliseconds
TIMEOUT_MS=$((TIMEOUT * 1000))

echo "xidle-autolock: Will run '$LOCKER' after ${TIMEOUT}s of inactivity"

# Main loop
while true; do
	# Get current idle time in milliseconds
	IDLE=$(xidle)
	
	# Check if idle time exceeds timeout
	if [ "$IDLE" -ge "$TIMEOUT_MS" ]; then
		echo "xidle-autolock: Idle threshold reached, locking screen..."
		$LOCKER
		
		# After locking, wait for user activity before checking again
		# This prevents immediately re-locking if unlock was quick
		sleep 5
	fi
	
	# Check every 5 seconds (adjust as needed)
	sleep 5
done
