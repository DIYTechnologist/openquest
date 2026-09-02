#!/system/bin/sh
# Diagnostic: after stopping trackingservice + the sensors HAL, does anything still hold
# /dev/video0 (msm-config)? The first bring-up run failed with EBUSY on exactly that node,
# while the sensor nodes (video3-6) enumerated fine.
LOG=/data/local/tmp/diag_video0.log
exec > "$LOG" 2>&1

holders() {
  for p in /proc/[0-9]*; do
    for f in $p/fd/*; do
      t=$(readlink "$f" 2>/dev/null)
      case "$t" in /dev/video0)
        echo "  HOLDER pid=${p#/proc/} cmd=$(tr '\0' ' ' < $p/cmdline 2>/dev/null)" ;;
      esac
    done
  done
}

echo "=== BEFORE STOP ==="
holders

setenforce 0
stop trackingservice
stop vendor.oculus.sensors-hal-1-0
setprop ctl.stop trackingservice
setprop ctl.stop vendor.oculus.sensors-hal-1-0

echo "=== POLLING for services to go down (15s) ==="
i=0
while [ $i -lt 15 ]; do
  ts=$(pgrep -f trackingservice | tr '\n' ' ')
  hal=$(pgrep -f sensors@1.0-service | tr '\n' ' ')
  echo "t=${i}s svc.tracking=$(getprop init.svc.trackingservice) svc.hal=$(getprop init.svc.vendor.oculus.sensors-hal-1-0) ts_pids='$ts' hal_pids='$hal'"
  [ -z "$ts" ] && [ -z "$hal" ] && break
  pkill -9 -f trackingservice
  pkill -9 -f sensors@1.0-service
  sleep 1
  i=$((i+1))
done

echo "=== AFTER STOP: /dev/video0 holders ==="
holders
echo "(none listed above = free)"

echo "=== can we open it? ==="
/data/local/tmp/cam_direct enum 2>&1 | head -30

echo "=== RESTORE ==="
start vendor.oculus.sensors-hal-1-0
sleep 4
start trackingservice
sleep 4
start trackingservice
setenforce 1
setenforce 1
getprop init.svc.trackingservice
getprop init.svc.vendor.oculus.sensors-hal-1-0
getenforce
echo "=== DONE ==="
