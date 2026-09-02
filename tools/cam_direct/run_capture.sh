#!/system/bin/sh
# VIO capture: cam0+cam2 (the pair validated by exports/vio-precise/trajectory_calib02.txt)
# plus the syncboss IMU, all from our own process. Run DETACHED; a watchdog restores the device.
LOG=/data/local/tmp/capture.log
exec > "$LOG" 2>&1

restore() {
  echo "=== RESTORE ==="
  start vendor.oculus.sensors-hal-1-0; sleep 3
  start; sleep 5
  start trackingservice; sleep 2; start trackingservice
  setenforce 1; setenforce 1
  echo "tracking=$(getprop init.svc.trackingservice) enforce=$(getenforce)"
}
trap 'restore; exit' HUP INT TERM

echo "=== START $(date) ==="
rm -rf /data/local/tmp/camdirect
setenforce 0
stop trackingservice
stop
sleep 5
stop vendor.oculus.sensors-hal-1-0

i=0
while [ $i -lt 20 ]; do
  [ -z "$(pidof vendor.oculus.hardware.sensors@1.0-service)" ] && break
  sleep 1; i=$((i+1))
done
if [ -n "$(pidof vendor.oculus.hardware.sensors@1.0-service)" ]; then
  echo "!! HAL still up, aborting"; restore; exit 1
fi
echo "--- services down at $(date)"

# Lead-in: get the headset ON and then HELD STILL before recording starts.
#
# This used to say "start looking around", which was wrong and cost us a whole capture. VIO
# initialisation (OpenVINS static init, and most others) requires the device to be stationary and
# then move off. Starting already in motion makes the filter initialise with zero velocity and
# gravity aligned to an accelerometer reading that contains real acceleration -- it then diverges,
# which is exactly what our first capture did. See notes/12.
echo "--- LEAD-IN 20s: put the headset on, then HOLD STILL (do not move yet)"
sleep 20

echo "--- CAPTURE START $(date) -- HOLD STILL for the first 4 s, THEN move"
# The first seconds must be stationary so the estimator can initialise; after that, translate
# (step side to side, lean in/out) rather than only rotating -- pure rotation gives no parallax.
/data/local/tmp/cam_direct capture 0 30 8000 255
echo "exit=$?"
echo "--- CAPTURE END $(date)"

echo "--- sizes"
ls /data/local/tmp/camdirect | wc -l
du -sh /data/local/tmp/camdirect
wc -l < /data/local/tmp/camdirect/frames.csv
ls -la /data/local/tmp/camdirect/syncboss.raw

restore
echo "=== DONE $(date) ==="
