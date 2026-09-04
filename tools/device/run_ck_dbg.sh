#!/system/bin/sh
exec > /data/local/tmp/ckdbg.log 2>&1
. /data/local/tmp/devctl.sh
ufs_pin
# Checked BEFORE stopping services: nothing updates sys.hmt.mounted once trackingservice is down.
# Warn-only -- camera captures do not need tracking, but a mismatch usually means the cover slipped.
require_covered warn
restore() { start vendor.oculus.sensors-hal-1-0; sleep 3; start; sleep 5; start trackingservice; sleep 2; start trackingservice; setenforce 1; echo RESTORED; }
trap 'restore; exit' HUP INT TERM
display_off                       # OLED burn-in protection. Left OFF afterwards too:
                                  # idle-with-cover-on is exactly when burn-in accumulates.
                                  # Re-enable with: echo 0 > /sys/class/graphics/fb0/blank
setenforce 0; stop trackingservice; stop; sleep 5; stop vendor.oculus.sensors-hal-1-0
i=0; while [ $i -lt 20 ]; do [ -z "$(pidof vendor.oculus.hardware.sensors@1.0-service)" ] && break; sleep 1; i=$((i+1)); done
echo "services down"
echo 0 > /proc/sys/kernel/kptr_restrict
echo "=== APP START ==="
/data/local/tmp/cam_kernel 4 6 3000 160
echo "=== APP EXIT=$? ==="
sync
restore
echo "DONE $(date)"
