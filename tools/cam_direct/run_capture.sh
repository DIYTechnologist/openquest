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
# TABLE START. Held on the head a "still" person still reads 0.125 m/s^2 of accelerometer
# excitation, and the move-off is gradual -- OpenVINS' static initialiser needs the window BEFORE
# the jerk to be quiet and the jerk itself to be sharp, and a head-worn gradual start satisfies
# neither (notes/12). Resting on a table gets excitation down to roughly the sensor noise floor
# and picking it up gives a genuine jerk.
echo "--- LEAD-IN 20s: leave the headset SITTING STILL ON A TABLE, facing a textured scene"
sleep 20

# Timed cues. The script runs detached, so these go to the log; watch them live with:
#   adb shell 'su -c "tail -f /data/local/tmp/capture.log"'
# Wall-clock instructions are unreliable here because the time to stop the framework and HAL
# varies by several seconds, so the cues are emitted relative to the actual recording start and
# the still window is made long enough that a few seconds of slop does not matter.
(
  sleep 1
  echo ""
  echo "=================================================="
  echo ">>> RECORDING. DO NOT TOUCH IT. Leave it on the table."
  echo "=================================================="
  n=15
  while [ $n -gt 0 ]; do
    echo "    ... hands off for $n more seconds"
    sleep 3
    n=$((n-3))
  done
  echo ""
  echo "=================================================="
  echo ">>> PICK IT UP NOW - and walk around for 20 s"
  echo ">>> translate: walk, step side to side, lean in and out"
  echo "=================================================="
) &
CUES=$!

echo "--- CAPTURE START $(date)"
/data/local/tmp/cam_direct capture 0 40 8000 255
echo "exit=$?"
kill $CUES 2>/dev/null
echo "--- CAPTURE END $(date)"

echo "--- sizes"
ls /data/local/tmp/camdirect | wc -l
du -sh /data/local/tmp/camdirect
wc -l < /data/local/tmp/camdirect/frames.csv
ls -la /data/local/tmp/camdirect/syncboss.raw

restore
echo "=== DONE $(date) ==="
