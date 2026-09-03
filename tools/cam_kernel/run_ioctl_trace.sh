#!/system/bin/sh
# Capture the reference ioctl trace: run the working B1 path (cam_direct -> libqcameraoculushal ->
# libqcameradriver) under the LD_PRELOAD tracer, so B2 has the exact call sequence to reimplement.
#
# Uses `all` so the trace covers every sensor, which is what the 4-camera end state needs anyway.
LOG=/data/local/tmp/camtrace.log
exec > "$LOG" 2>&1

restore() {
  echo "=== RESTORE ==="
  start vendor.oculus.sensors-hal-1-0; sleep 3
  start; sleep 5
  start trackingservice; sleep 2; start trackingservice
  setenforce 1; setenforce 1
  echo "tracking=$(getprop init.svc.trackingservice) hal=$(getprop init.svc.vendor.oculus.sensors-hal-1-0) enforce=$(getenforce)"
}
trap 'echo "!! trapped signal"; restore; exit' HUP INT TERM

echo "=== START $(date) ==="
setenforce 0
stop trackingservice
stop
sleep 5
stop vendor.oculus.sensors-hal-1-0

# Never pgrep -f/pkill -f a service name here: the pattern matches the shell running this script.
i=0
while [ $i -lt 20 ]; do
  [ -z "$(pidof vendor.oculus.hardware.sensors@1.0-service)" ] && break
  sleep 1; i=$((i+1))
done
if [ -n "$(pidof vendor.oculus.hardware.sensors@1.0-service)" ]; then
  echo "!! HAL still up, aborting"; restore; exit 1
fi
echo "--- services down at $(date)"

rm -f /data/local/tmp/ioctl_trace.log
export IOCTL_TRACE_OUT=/data/local/tmp/ioctl_trace.log
export LD_PRELOAD=/data/local/tmp/libioctl_trace.so
/data/local/tmp/cam_direct all 0 3 3000 160
echo "exit=$?"
unset LD_PRELOAD

echo "--- trace size"
wc -l /data/local/tmp/ioctl_trace.log
echo "--- distinct ioctls"
awk '{print $4}' /data/local/tmp/ioctl_trace.log | grep -v '^#' | sort | uniq -c | sort -rn

restore
echo "=== DONE $(date) ==="
