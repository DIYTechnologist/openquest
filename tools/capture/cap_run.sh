#!/system/bin/sh
# Step 2 synchronised capture: Meta's frames, the MCU IMU, and Meta's own poses, in one session on
# one clock (CLOCK_MONOTONIC).  Usage: cap_run.sh [seconds]
#
# Assumes ts_ibfs2.sh has already restarted trackingservice under the leech and it has warmed up.
# Creating cap/GO is what opens the frame dump gate, so all three streams start together here.
D=${1:-150}
CAP=/data/local/tmp/cap

echo "[cap] $(date) duration=${D}s"
[ -f /data/local/tmp/ibfs_hook2.so ] || { echo "[cap] FATAL: hook missing"; exit 1; }
grep -q ibfs_hook2 /proc/$(pgrep -f '^/system/bin/trackingservice' | head -1)/maps 2>/dev/null \
  || { echo "[cap] FATAL: trackingservice is not running under the leech"; exit 1; }

# IMU and Meta poses run for slightly longer than the frame window, so the frame interval is fully
# bracketed by both and no edge alignment is lost.
DA=$((D + 10))
/data/local/tmp/sb_leech "$DA" "$CAP/imu.bin"  > "$CAP/imu.txt"  2>&1 &
IMU=$!
/data/local/tmp/pose_log 60 "$DA" > "$CAP/meta_poses.csv" 2> "$CAP/pose.err" &
POSE=$!
sleep 3

touch "$CAP/GO"
echo "[cap] GO at $(cat /proc/uptime | cut -d' ' -f1) -- dumping frames"
i=0
while [ "$i" -lt "$D" ]; do
  sleep 10
  i=$((i + 10))
  echo "[cap]   t=${i}s frames=$(wc -l < "$CAP/ib.idx" 2>/dev/null || echo 0) blob=$(du -m "$CAP/ib_frames.bin" 2>/dev/null | cut -f1)MB"
done
rm -f "$CAP/GO"
echo "[cap] frame dump closed"

wait $IMU 2>/dev/null
wait $POSE 2>/dev/null
sync

echo "[cap] ===== SUMMARY ====="
echo "[cap] frames dumped : $(wc -l < "$CAP/ib.idx" 2>/dev/null || echo 0)"
echo "[cap] per cam       : $(awk '{c[$6]++} END{for(k in c) printf "cam%s=%d ",k,c[k]}' "$CAP/ib.idx" 2>/dev/null)"
echo "[cap] blob size     : $(du -m "$CAP/ib_frames.bin" 2>/dev/null | cut -f1) MB"
echo "[cap] IB log lines  : $(grep -c '^IB'  "$CAP/ib.log" 2>/dev/null || echo 0)"
echo "[cap] FS log lines  : $(grep -c '^FS'  "$CAP/ib.log" 2>/dev/null || echo 0)"
echo "[cap] meta poses    : $(grep -vc '^#' "$CAP/meta_poses.csv" 2>/dev/null || echo 0)"
grep -E 'samples|distinct' "$CAP/pose.err" 2>/dev/null
grep -E 'stream|VERDICT|0x50' "$CAP/imu.txt" 2>/dev/null
echo "[cap] done"
