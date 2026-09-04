#!/system/bin/sh
# Restart trackingservice under the capture-grade leech (ibfs_hook2.so), then idle.
#
# Deliberately does NOT create cap/GO. The old ts_ibfs1.sh created it immediately, which starts
# dumping the instant the service comes up -- fine for a 10 s probe, wrong here: the tracker needs
# to warm up and the user needs time to put the headset on, and we only have IBFS_MAX frames of
# budget. cap_run.sh creates GO when the capture actually starts.
#
# Run detached, or the stop/pkill cascade kills the adb shell that launched it:
#   adb shell 'su -c "setsid sh /data/local/tmp/ts_ibfs2.sh </dev/null >/dev/null 2>&1 &"'
setenforce 0
rm -rf /data/local/tmp/cap
mkdir -p /data/local/tmp/cap
# IBFS_PREGO=1 opens the dump gate before the service starts, so the ~0.5 s burst of frames that
# trackingservice emits at startup gets written. That burst is the only time real pixels flow with
# the headset off a head, so it is the only way to validate the writer without a worn session.
[ "${IBFS_PREGO:-0}" = "1" ] && touch /data/local/tmp/cap/GO
stop trackingservice
sleep 1
pkill -9 -f trackingservice
sleep 1
export LD_PRELOAD=/data/local/tmp/ibfs_hook2.so
export IBFS_MAX=${IBFS_MAX:-12000}
export IBFS_CAMMASK=${IBFS_CAMMASK:-0x3}
export IBFS_DUMPEMPTY=${IBFS_DUMPEMPTY:-0}
export IBFS_MAXFS=${IBFS_MAXFS:-12000}
exec /system/bin/trackingservice > /data/local/tmp/ts_out.log 2>&1
