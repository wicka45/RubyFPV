#!/bin/bash
# RubyFPV ground station, WINDOWED on the desktop (X / XWayland). Launched from the GNOME menu or from
# a terminal inside your graphical session. Everything runs from /opt/rubyfpv as the invoking user:
# the ruby_* binaries carry file capabilities for the radio, ruby_central auto-detects DISPLAY and
# renders into a window, and ruby_start forces the video player headless so video composites into that
# one window. Only the NetworkManager / iw radio prep uses sudo (passwordless, non-interactive).
# Window size: RUBY_WINDOW_W / RUBY_WINDOW_H (default 1280x720). Force the DRM kiosk: RUBY_FORCE_DRM=1.
cd /opt/rubyfpv || exit 1
if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
  echo "No DISPLAY/WAYLAND_DISPLAY -- run inside your desktop session." >&2
  exit 1
fi

# Internal render resolution (per-host CPU/battery knob): OSD + video composite at this size, the GPU upscales
# to the window/panel. woody = 1080p (native panel 2880x1800). Lower on weaker laptops (e.g. 1280 720).
export RUBY_WINDOW_W=1920
export RUBY_WINDOW_H=1080

# Pick the RubyFPV radio NIC by chipset and keep NetworkManager off it (root via passwordless sudo).
RUBY_WLAN_IFACE=""
for ifpath in /sys/class/net/*; do
  ifn=$(basename "$ifpath")
  { [ -d "$ifpath/wireless" ] || [ -e "$ifpath/phy80211" ]; } || continue
  drv=$(basename "$(readlink -f "$ifpath/device/driver" 2>/dev/null)" 2>/dev/null)
  drvlc=$(echo "$drv" | tr "A-Z" "a-z")
  case "$drvlc" in
    *88xxau*|*8812au*|*8814au*|*8811au*|*8821au*|ath9k_htc|carl9170|*mt76x2u*|*mt7601u*|*mt7921u*)
      RUBY_WLAN_IFACE="$ifn"; break ;;
  esac
done
if [ -n "$RUBY_WLAN_IFACE" ]; then
  export RUBY_WLAN_IFACE
  sudo -n nmcli dev set "$RUBY_WLAN_IFACE" managed no 2>/dev/null
  sudo -n pkill -f "wpa_supplicant.*$RUBY_WLAN_IFACE" 2>/dev/null
else
  echo "WARN: no supported RubyFPV radio NIC found (plug the dongle)." >&2
fi
sudo -n iw reg set US 2>/dev/null && sleep 1

cleanup() {
  trap - EXIT INT TERM HUP
  [ -n "${RUBY_INHIBIT_PID:-}" ] && kill "$RUBY_INHIBIT_PID" 2>/dev/null   # release the idle/sleep inhibitor
  xset s on 2>/dev/null; xset +dpms 2>/dev/null                           # restore the session's normal blanking/DPMS
  sudo -n sh -c '[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo' 2>/dev/null  # restore CPU turbo
  pkill -9 -f "ruby_start|ruby_central|ruby_rt_station|ruby_rx_telemetry|ruby_tx_rc|ruby_logger|ruby_player" 2>/dev/null
  sleep 1
  sudo -n rm -f /dev/shm/RUBY* /dev/shm/SSMR* /dev/shm/SYSTEM* /dev/shm/R_* /dev/shm/sem.* 2>/dev/null
  exit 0
}
trap cleanup EXIT INT TERM HUP

# Keep the screen awake while the GS runs (don't let it blank/dim/sleep mid-flight). GNOME (X or Wayland)
# honors gnome-session-inhibit; systemd-inhibit covers logind sleep; xset covers plain XWayland DPMS.
RUBY_INHIBIT_PID=""
if command -v gnome-session-inhibit >/dev/null 2>&1; then
  gnome-session-inhibit --inhibit idle:suspend --reason "RubyFPV ground station running" sleep infinity &
  RUBY_INHIBIT_PID=$!
elif command -v systemd-inhibit >/dev/null 2>&1; then
  systemd-inhibit --what=idle:sleep --who="RubyFPV" --why="Ground station running" --mode=block sleep infinity &
  RUBY_INHIBIT_PID=$!
fi
xset s off 2>/dev/null; xset s noblank 2>/dev/null; xset -dpms 2>/dev/null

# Cap CPU turbo while the GS runs: base clock handles 1080p60 (HW decode + GPU scale) without saturating,
# and dropping the turbo voltage cuts ~10C on thermally-marginal laptops (e.g. the 2014 MBP). Restored on
# exit (cleanup), so builds/other work keep turbo. Opt out with RUBY_NO_TURBO_CAP=1. No-op where unsupported.
[ -z "${RUBY_NO_TURBO_CAP:-}" ] && sudo -n sh -c '[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo' 2>/dev/null

./ruby_start
# ruby_start bootstraps the stack and returns; hold until ruby_central (the windowed UI) ends, then clean up.
for i in $(seq 1 30); do pgrep -x ruby_central >/dev/null 2>&1 && break; sleep 1; done
while pgrep -x ruby_central >/dev/null 2>&1; do sleep 2; done
