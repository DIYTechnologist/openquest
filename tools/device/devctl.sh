#!/system/bin/sh
# devctl.sh — small device helpers shared by every capture script.
#
#   display_off / display_on   OLED burn-in protection.
#   require_covered            refuse to capture unless the proximity sensor reads "worn".
#   ufs_pin                    keep the UFS link out of idle clock-gating (notes/19).
#
# Usage:  . /data/local/tmp/devctl.sh   (source it, don't exec it)

# ── UFS ──────────────────────────────────────────────────────────────────────────────────────
# The link dies during idle hibern8 and never recovers; after that adb push reports success while
# writing only page cache, which silently invalidates a capture. See notes/19 session 6.
ufs_pin() {
  echo 0 > /sys/devices/soc/1da4000.ufshc/clkgate_enable 2>/dev/null
  echo 0 > /sys/devices/soc/1da4000.ufshc/hibern8_on_idle_enable 2>/dev/null
}

# ── Display ──────────────────────────────────────────────────────────────────────────────────
# These are OLED panels, so a static bright image burns in. Blanking the panel is reversible and
# costs nothing during a camera capture.
#
# IMPORTANT: blanking STOPS HEAD TRACKING (measured: 6DOF -> 0DOF Valid:No, even with the prox
# sensor still reading covered). So anything that needs poses must run with the panel on. Camera
# captures do not need poses and should blank.
display_off() { echo 4 > /sys/class/graphics/fb0/blank 2>/dev/null; }   # FB_BLANK_POWERDOWN
display_on()  { echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null; }   # FB_BLANK_UNBLANK

# ── Proximity / mount state ──────────────────────────────────────────────────────────────────
# sys.hmt.mounted is driven by the MCU's PROXSTATE (207) message. It is an OUTPUT: setprop does not
# fake it (measured -- tracking stays 0DOF). Must be checked BEFORE services are stopped, because
# nothing updates the property once trackingservice is down.
#
# $1 = "hard" to abort, anything else to warn only.
require_covered() {
  m=$(getprop sys.hmt.mounted)
  if [ "$m" = "1" ]; then
    echo "[+] proximity: COVERED (sys.hmt.mounted=1)"
    return 0
  fi
  echo "[-] proximity: NOT COVERED (sys.hmt.mounted='$m')"
  echo "[-] The headset reads as off-head, so tracking is gated to 0DOF and"
  echo "[-] getHeadTrackingData returns {}. Cover the proximity sensor and retry."
  [ "$1" = "hard" ] && exit 90
  return 1
}
