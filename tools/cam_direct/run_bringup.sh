#!/system/bin/sh
# B1 live bring-up runner. Run DETACHED (setsid) — stopping the sensors HAL cascades
# (onrestart: calibration_svr, mrsystemservice, sensorproxy) and can SIGKILL the adb shell.
# Everything is logged to $LOG; restore always runs, even if a stage aborts.
LOG=/data/local/tmp/camdirect.log
exec > "$LOG" 2>&1

echo "=== START $(date) ==="
logcat -c 2>/dev/null

echo "--- pre-state"
getprop init.svc.trackingservice
getprop init.svc.vendor.oculus.sensors-hal-1-0
getenforce

setenforce 0
echo "--- stopping consumers of the camera pipeline"
stop trackingservice
stop vendor.oculus.sensors-hal-1-0
sleep 3
pkill -9 -f trackingservice
pkill -9 -f sensors@1.0-service
sleep 2
echo "trackingservice pids: $(pgrep -f trackingservice | tr '\n' ' ')"
echo "sensors-hal pids:     $(pgrep -f sensors@1.0-service | tr '\n' ' ')"

echo
echo "########## STAGE 1: enum"
/data/local/tmp/cam_direct enum
echo "exit=$?"

echo
echo "########## STAGE 2: start sensor 0"
/data/local/tmp/cam_direct start 0
echo "exit=$?"

echo
echo "########## STAGE 3: stream 10 frames from sensor 0"
/data/local/tmp/cam_direct stream 0 10
echo "exit=$?"

echo
echo "########## VENDOR LOGCAT"
logcat -d 2>/dev/null | grep -iE "QCameraOculusHAL|qcamera|CameraDriver|msm_|camera" | tail -60

echo
echo "########## RESTORE"
start vendor.oculus.sensors-hal-1-0
sleep 4
start trackingservice
sleep 4
start trackingservice   # documented: sometimes needs a second try
setenforce 1
setenforce 1            # documented: may need repeating
echo "--- post-state"
getprop init.svc.trackingservice
getprop init.svc.vendor.oculus.sensors-hal-1-0
getenforce
echo "=== DONE $(date) ==="
