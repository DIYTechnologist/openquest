#!/system/bin/sh
# Unconditional restore watchdog. Runs as its own process (setsid) so that a SIGKILL of the
# capture script -- or any mid-script death -- cannot strand the device with trackingservice, the
# framework and the sensors HAL stopped and SELinux permissive.
#
# Usage: setsid sh watchdog.sh [seconds] &      (default 300)
SECS=${1:-300}
LOG=/data/local/tmp/watchdog.log
exec > "$LOG" 2>&1
echo "=== watchdog armed for ${SECS}s at $(date) ==="
sleep "$SECS"
echo "=== watchdog firing at $(date) ==="
start vendor.oculus.sensors-hal-1-0
sleep 3
start
sleep 5
start trackingservice
sleep 2
start trackingservice
setenforce 1
setenforce 1
echo "tracking=$(getprop init.svc.trackingservice) hal=$(getprop init.svc.vendor.oculus.sensors-hal-1-0) enforce=$(getenforce)"
echo "=== watchdog done $(date) ==="
