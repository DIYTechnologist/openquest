#!/system/bin/sh
# Step 2 motion capture: stereo frames (cams 0+2) + Meta poses + IMU, one clock.
# Usage: cap9_run.sh [seconds]
D=${1:-75}
STATIC=${2:-20}
CAP=/data/local/tmp/cap9
P=$(pgrep -f '^/system/bin/trackingservice' | head -1)
grep -q ibfs_hook9 /proc/$P/maps 2>/dev/null || { echo "[cap] FATAL: TS not under hook9"; exit 1; }

# Re-assert the proximity override throughout. Earlier runs captured only ~9 s of a 20 s window
# with a single up-front broadcast, which is consistent with the override lapsing and tracking
# falling back to standby -- frames within the 9 s were gapless, so it ends the session rather than
# dropping frames.
am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
sleep 2
echo "[cap] tracking: $(dumpsys tracking 2>/dev/null | grep -o '6DOF\|3DOF\|0DOF' | head -1)"
# STATIC LEAD-IN, RECORDED. OpenVINS static init estimates gyro/accel bias from a stationary
# window and only then triggers on motion; a capture that moves from frame one never initialises
# cleanly. The first attempt did exactly that and diverged to 95-196 m, initialising 74 s in on a
# marginal window. notes/22's successful run had ~54 s of stillness first, which was accidental.
# So: leave the headset ON THE DESK, untouched, while the first STATIC seconds are recorded.
DA=$((D + STATIC + 10))
/data/local/tmp/sb_leech "$DA" "$CAP/imu.bin" > "$CAP/imu.txt" 2>&1 &
/data/local/tmp/pose_log 60 "$DA" > "$CAP/meta_poses.csv" 2> "$CAP/pose.err" &
sleep 1
touch "$CAP/GO"
echo "[cap] RECORDING - LEAVE IT COMPLETELY STILL ON THE DESK for ${STATIC}s"
i=0
while [ $i -lt "$STATIC" ]; do
  sleep 5; i=$((i+5))
  am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
  echo "[cap]  static t=${i}/${STATIC}s  (do not touch)"
done
echo "[cap] >>> NOW PICK IT UP AND KEEP MOVING for ${D}s <<<"
i=0
while [ $i -lt "$D" ]; do
  sleep 5; i=$((i+5))
  am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
  echo "[cap]  t=${i}s frames=$(wc -l < "$CAP/frames.idx" 2>/dev/null || echo 0) $(du -m "$CAP/frames.bin" 2>/dev/null | cut -f1)MB poses=$(grep -vc '^#' "$CAP/meta_poses.csv" 2>/dev/null || echo 0)"
done
rm -f "$CAP/GO"
echo "[cap] STOP - you can put it down"
wait
sync
echo "[cap] ===== SUMMARY ====="
echo "[cap] rows        : $(wc -l < "$CAP/frames.idx")"
awk '{c[$2]++} END{printf "[cap] per cam     : "; for(k in c) printf "cam%s=%d ",k,c[k]; print ""}' "$CAP/frames.idx"
echo "[cap] unique stamps: $(awk '{print $2"_"$3}' "$CAP/frames.idx" | sort -u | wc -l)"
echo "[cap] blob        : $(du -m "$CAP/frames.bin" | cut -f1) MB"
echo "[cap] meta poses  : $(grep -vc '^#' "$CAP/meta_poses.csv")"
grep -E 'samples|distinct' "$CAP/pose.err" 2>/dev/null
grep -E '0x50|VERDICT' "$CAP/imu.txt" 2>/dev/null
