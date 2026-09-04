#!/system/bin/sh
# Put the device back the way we found it. Headset usability takes priority over keeping evidence,
# per the standing rule in notes/10 -- but PULL THE CAPTURE FIRST: cap/ is not regenerable without
# another worn session.
rm -f /data/local/tmp/cap/GO
stop trackingservice
sleep 1
pkill -9 -f trackingservice
sleep 1
start trackingservice
sleep 3
setenforce 1
echo "[restore] getenforce=$(getenforce)"
echo "[restore] trackingservice pid=$(pgrep -f '^/system/bin/trackingservice' | head -1)"
echo -n "[restore] preload still mapped? "
P=$(pgrep -f '^/system/bin/trackingservice' | head -1)
if [ -n "$P" ] && grep -qc ibfs_hook /proc/"$P"/maps 2>/dev/null; then echo "YES (bad)"; else echo "no (clean)"; fi
