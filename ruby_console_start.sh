#!/bin/bash
# Run the RubyFPV GS from the PHYSICAL console (tty1).
#   - a real tty (/dev/tty1) satisfies ruby_start's console check
#   - run as: sudo /opt/rubyfpv/ruby_console_start.sh  (root -> radio ip/iw + DRM; tty1 = DRM master)
# Supervises the session: ruby_start bootstraps the stack and exits, so we wait for the GS and, on
# exit (GS quit, or Ctrl+C here), auto-clean ruby_* + shared memory/semaphores. No ruby_stop_gs needed.
cd /opt/rubyfpv || exit 1

# Console GPU present (EGL-on-GBM GLES2) is the DEFAULT (auto-falls back to the software path on any
# failure). Disable with:  sudo touch /opt/rubyfpv/config/no_drm_gl   (or RUBY_DRM_GL=0 in the env).
[ -e /opt/rubyfpv/config/no_drm_gl ] && export RUBY_DRM_GL=0
[ -n "${RUBY_DRM_GL:-}" ] && export RUBY_DRM_GL

# Internal render resolution (per-host CPU/battery knob): OSD + video composite at this size, the GPU upscales
# to the panel (letterboxed if the aspect differs, e.g. 1080p 16:9 on a 2880x1800 16:10 panel). woody = 1080p.
# Unset -> render at the native panel mode. Lower on weaker laptops (e.g. 1280 720).
export RUBY_WINDOW_W=1920
export RUBY_WINDOW_H=1080

# --- Intelligently pick the RubyFPV radio NIC by CHIPSET (not a hardcoded name) ---------------------
# RubyFPV only supports a few injection chipsets (rtl8812au/8814au/8811au/8821au, ath9k_htc, mt76xx).
# Identify THAT NIC and unmanage only it from NetworkManager; never touch the onboard wifi (which may
# itself be wlan0, e.g. on a MacBook Air). The dongle's bound driver is e.g. "rtl88XXau".
RUBY_WLAN_IFACE=""
for ifpath in /sys/class/net/*; do
  ifn=$(basename "$ifpath")
  { [ -d "$ifpath/wireless" ] || [ -e "$ifpath/phy80211" ]; } || continue   # wireless only
  drv=$(basename "$(readlink -f "$ifpath/device/driver" 2>/dev/null)" 2>/dev/null)
  drvlc=$(echo "$drv" | tr 'A-Z' 'a-z')
  case "$drvlc" in
    *88xxau*|*8812au*|*8814au*|*8811au*|*8821au*|ath9k_htc|carl9170|*mt76x2u*|*mt7601u*|*mt7921u*)
      RUBY_WLAN_IFACE="$ifn"; RUBY_WLAN_DRIVER="$drv"; break ;;
  esac
done
if [ -z "$RUBY_WLAN_IFACE" ]; then
  echo "WARN: no supported RubyFPV radio NIC found (rtl8812au etc.). Plug the dongle. NOT touching onboard wifi."
else
  export RUBY_WLAN_IFACE
  # Keep NetworkManager/wpa_supplicant OFF the radio NIC (they grab it + scan it off-channel).
  nmcli dev set "$RUBY_WLAN_IFACE" managed no 2>/dev/null
  pkill -f "wpa_supplicant.*$RUBY_WLAN_IFACE" 2>/dev/null
fi

# Regulatory domain US (resets to world(00) each boot; world blocks the 5600/ch120 DFS link).
iw reg set US 2>/dev/null && sleep 1

# Pick the atomic-capable DRM card driving the connected internal panel (i915 on these Intel laptops).
for c in /sys/class/drm/card[0-9]*; do
  [ -e "$c/device/driver" ] || continue
  drv=$(readlink -f "$c/device/driver" 2>/dev/null | xargs basename)
  if [ "$drv" = i915 ] && grep -qx connected "$c"/*/status 2>/dev/null; then
    export RUBY_DRI_CARD="/dev/dri/$(basename "$c")"; break
  fi
done
export RUBY_DRI_CARD="${RUBY_DRI_CARD:-/dev/dri/card1}"

# --- Auto-cleanup on exit (replaces manual ruby_stop_gs.sh) ---
cleanup() {
  trap - EXIT INT TERM HUP
  [ -n "${RUBY_INHIBIT_PID:-}" ] && kill "$RUBY_INHIBIT_PID" 2>/dev/null   # release the idle/sleep inhibitor
  [ -e /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null  # restore CPU turbo
  echo "RubyFPV GS: stopping (kill ruby_* + clear /dev/shm + semaphores)..."
  pkill -9 -f "ruby_start|ruby_controller|ruby_central|ruby_rt_station|ruby_rx_telemetry|ruby_tx_rc|ruby_logger|ruby_player" 2>/dev/null
  pkill -9 -f "setsid bash.*ruby_player|sudo setsid bash" 2>/dev/null
  sleep 1
  rm -f /dev/shm/RUBY* /dev/shm/SSMR* /dev/shm/SYSTEM* /dev/shm/R_* /dev/shm/sem.* 2>/dev/null
  echo "RubyFPV GS: cleanup complete."
  exit 0
}
trap cleanup EXIT INT TERM HUP

echo "RubyFPV GS: tty=$(tty)  user=$(id -un)  card=$RUBY_DRI_CARD  radio=${RUBY_WLAN_IFACE:-<none>} (${RUBY_WLAN_DRIVER:-?})"

# Keep the screen awake while the GS runs on the console. The DRM master won't blank the active output
# while it page-flips, but the kernel VT blanker + logind can still kick in -> disable both.
setterm --blank 0 --powerdown 0 2>/dev/null || true
RUBY_INHIBIT_PID=""
if command -v systemd-inhibit >/dev/null 2>&1; then
  systemd-inhibit --what=idle:sleep --who="RubyFPV" --why="Ground station running" --mode=block sleep infinity &
  RUBY_INHIBIT_PID=$!
fi

# Cap CPU turbo while the GS runs (cooler on thermally-marginal laptops; base clock handles 1080p60). Restored on exit. Opt out: RUBY_NO_TURBO_CAP=1.
[ -z "${RUBY_NO_TURBO_CAP:-}" ] && [ -e /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null

./ruby_start

# ruby_start bootstraps + exits; hold the session until the GS (ruby_central) ends -> trap cleans up.
for i in $(seq 1 30); do pgrep -x ruby_central >/dev/null 2>&1 && break; sleep 1; done
while true; do
  if ! pgrep -x ruby_central >/dev/null 2>&1; then
    sleep 3
    pgrep -x ruby_central >/dev/null 2>&1 || break
  fi
  sleep 2
done
