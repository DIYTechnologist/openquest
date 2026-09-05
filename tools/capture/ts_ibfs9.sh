#!/system/bin/sh
setenforce 0
# Do NOT rm -rf the capture dir: it destroys any prior capture still waiting to be pulled
# (lost a 4.7 GB capture that way). Remove only this run's own outputs.
rm -f /data/local/tmp/cap9/frames.bin /data/local/tmp/cap9/frames.idx /data/local/tmp/cap9/cap.log
mkdir -p /data/local/tmp/cap9
stop trackingservice
sleep 1
pkill -9 -f trackingservice
sleep 1
export LD_PRELOAD=/data/local/tmp/ibfs_hook9.so
export IBFS_MAXFS=${IBFS_MAXFS:-4000}
exec /system/bin/trackingservice > /data/local/tmp/ts9_out.log 2>&1
