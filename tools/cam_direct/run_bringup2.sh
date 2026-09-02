#!/system/bin/sh
# B1 live bring-up, attempt 2.
#
# Why this is heavier than attempt 1: /dev/video0 (msm-config) is STRICTLY SINGLE-OPEN
# (msm.c:1091, atomic_cmpxchg -> -EBUSY), and the sensors HAL holds it. `stop` on the HAL alone
# does not hold, because the same process also serves android.hardware.sensors@2.0::ISensors,
# so the framework's sensorservice makes hwservicemanager/init respawn it. Hence: stop the
# framework first, then the HAL.
#
# No pkill and no `setprop ctl.stop` this time — attempt 1's diagnostic died partway using those.
# A separate watchdog restores the device unconditionally, so a mid-script death can't strand it.
LOG=/data/local/tmp/camdirect2.log
exec > "$LOG" 2>&1

restore() {
  echo "=== RESTORE ==="
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
}
trap 'echo "!! trapped signal"; restore; exit' HUP INT TERM

echo "=== START $(date) ==="
logcat -c 2>/dev/null
echo "pre: tracking=$(getprop init.svc.trackingservice) hal=$(getprop init.svc.vendor.oculus.sensors-hal-1-0) enforce=$(getenforce)"

setenforce 0
echo "--- stopping trackingservice + framework + sensors HAL"
stop trackingservice
stop                      # framework (zygote/system_server) — the sensors@2.0 client
sleep 5
stop vendor.oculus.sensors-hal-1-0

echo "--- polling for the HAL to stay down (20s)"
i=0
while [ $i -lt 20 ]; do
  hp=$(pidof vendor.oculus.hardware.sensors@1.0-service)
  echo "t=${i}s hal_state=$(getprop init.svc.vendor.oculus.sensors-hal-1-0) hal_pid='$hp'"
  [ -z "$hp" ] && break
  sleep 1
  i=$((i + 1))
done

if [ -n "$(pidof vendor.oculus.hardware.sensors@1.0-service)" ]; then
  echo "!! HAL still up — aborting before we touch the pipeline"
  restore
  echo "=== DONE (aborted) $(date) ==="
  exit 1
fi
echo "--- HAL is down; /dev/video0 should be free"

echo
echo "########## STAGE 1: enum"
/data/local/tmp/cam_direct enum; echo "exit=$?"

echo
echo "########## (start stage folded into stage 4)"

echo
echo "########## STAGE 4: all four cameras (6 rounds, exp=300 gain=100)"
# cfg variant 8 (cam_format=112, fourcc 'GREY' mono8) is the one that returns rc=0.
# (42 passes the range check but has no V4L2 mapping -> 'Unknown fmt=42' and no frames.)
# Re-assert the HAL is down first: a stale watchdog can restart it mid-run.
stop vendor.oculus.sensors-hal-1-0
sleep 2
echo "hal before stream: '$(pidof vendor.oculus.hardware.sensors@1.0-service)'"
/data/local/tmp/cam_direct all 0 6 8000 255; echo "exit=$?"

echo
echo "########## dmesg (camera power gating via syncboss?)"
dmesg | grep -iE "syncboss|Turning on cameras|Turning off cameras|csid|csiphy" | tail -25

echo
echo "########## VENDOR LOGCAT"
logcat -d 2>/dev/null | grep -iE "QCamera|CameraDriver|DEBUG|msm" | tail -60

restore
echo "=== DONE $(date) ==="
