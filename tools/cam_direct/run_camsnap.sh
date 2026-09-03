#!/system/bin/sh
# Static snapshot from ALL FOUR tracking cameras, for measuring inter-camera overlap.
#
# Why: every VIO capture so far used cam0+cam2 only, because stage_capture hardcodes camB = camA+2
# (cam_direct.c). That pair was chosen on the strength of the 0.378 m trajectory that notes/08
# later retracted, and was never revisited. Before extending capture to 4 cameras it is worth
# knowing, from real images, which pairs actually share field of view.
#
# The headset just sits still for this -- no handling needed, so there is no lead-in or cue.
# Sweeps a few exposure/gain settings because the room brightness is unknown and the SLAM capture's
# 8000/255 will saturate a bright room; pick the best set afterwards on the host.
LOG=/data/local/tmp/camsnap.log
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
echo "pre: tracking=$(getprop init.svc.trackingservice) enforce=$(getenforce)"

setenforce 0
echo "--- stopping trackingservice + framework + sensors HAL"
stop trackingservice
stop
sleep 5
stop vendor.oculus.sensors-hal-1-0

# /dev/video0 is single-open and the same process serves android.hardware.sensors@2.0, so the
# framework must be down too or init respawns the HAL. Never pgrep -f/pkill -f here: the pattern
# matches the shell running this script.
i=0
while [ $i -lt 20 ]; do
  [ -z "$(pidof vendor.oculus.hardware.sensors@1.0-service)" ] && break
  sleep 1; i=$((i+1))
done
if [ -n "$(pidof vendor.oculus.hardware.sensors@1.0-service)" ]; then
  echo "!! HAL still up, aborting"; restore; exit 1
fi
echo "--- services down at $(date)"

rm -rf /data/local/tmp/camsnap
mkdir -p /data/local/tmp/camsnap

for eg in "300 40" "1000 80" "3000 160" "8000 255"; do
  set -- $eg
  EXP=$1; GAIN=$2
  echo ""
  echo "########## exposure=$EXP gain=$GAIN"
  rm -rf /data/local/tmp/camdirect
  stop vendor.oculus.sensors-hal-1-0     # re-assert: a stale watchdog can restart it mid-run
  sleep 1
  /data/local/tmp/cam_direct all 0 3 "$EXP" "$GAIN"
  echo "exit=$?"
  if [ -d /data/local/tmp/camdirect ]; then
    mv /data/local/tmp/camdirect "/data/local/tmp/camsnap/e${EXP}_g${GAIN}"
    ls "/data/local/tmp/camsnap/e${EXP}_g${GAIN}"
  fi
done

echo ""
echo "--- results"
du -sh /data/local/tmp/camsnap
find /data/local/tmp/camsnap -name '*.gray' | wc -l

restore
echo "=== DONE $(date) ==="
