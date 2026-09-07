#!/system/bin/sh
# Controller button capture. Records the raw syncboss stream while the user works through a
# scripted button order; the ORDER is the ground truth, so no live cue or reference logger is
# needed (Meta's getcontrollerbuttondata is 2.7 s/call and unusable for this -- notes/31).
D=${1:-240}
OUT=${2:-/data/local/tmp/ctl_capture.bin}
am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
sleep 2
/data/local/tmp/sb_leech "$D" "$OUT" > /data/local/tmp/ctl_capture.txt 2>&1 &
LP=$!
i=0
while [ $i -lt "$D" ]; do
  sleep 10; i=$((i+10))
  am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
  echo "[ctl] t=${i}/${D}s  $(du -k "$OUT" 2>/dev/null | cut -f1)KB"
done
wait $LP
sync
echo "[ctl] done: $(ls -l "$OUT" | awk '{print $5}') bytes"
grep -E '0x8f|0xd9|VERDICT' /data/local/tmp/ctl_capture.txt
