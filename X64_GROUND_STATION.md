# RubyFPV x86-64 Ground Station

Run RubyFPV as a **ground station (GS)** on a generic x86-64 Linux laptop or PC,
instead of a Raspberry Pi / Radxa. Built with `RUBY_BUILD_ENV=x64`.

Two output modes, auto-detected at launch:

- **Console** — direct DRM/KMS, no desktop environment (kiosk-style).
- **Windowed** — a resizable window on X or XWayland (auto-selected when `DISPLAY`
  or `WAYLAND_DISPLAY` is set).

Video is hardware-decoded via VA-API where possible; the OSD + video are composited
on the GPU (EGL/GLES2) with an automatic software fallback.

> Status: developed and run on Intel-graphics laptops (see
> [Hardware compatibility](#1-hardware-compatibility)). Treat other hardware as
> untested — the software fallbacks should still work, but please report results.

---

## 1. Hardware compatibility

### GPU / display

| Requirement | Notes |
|---|---|
| DRM/KMS device with **atomic** modesetting | Needed for the console backend. |
| **EGL + OpenGL ES 2.0** (+ **GBM** on console) | For the GPU compositor. Provided by Mesa. |

- **Verified:** Intel `i915` — Iris Pro 5200 (Haswell, MacBookPro11,3),
  HD4400 (Haswell, Lenovo T440s), HD3000 (Sandy Bridge, MacBookAir4,2;
  VA-API via `i965`).
- **Should work:** any Mesa driver exposing atomic KMS + GLES2/EGL/GBM (Intel, AMD).
- **NVIDIA `nouveau` does NOT work** for the console backend — it is legacy-KMS only
  (no atomic modeset). On a dual-GPU laptop the active panel must be driven by an
  atomic-capable GPU (see [Appendix: dual-GPU MacBooks](#appendix-dual-gpu-macbook-display-routing)).
- **No GPU / GL unavailable:** the GS falls back to a **CPU (cairo) compositor** and
  still runs on any KMS device (including Mesa `llvmpipe`), at higher CPU/heat. A
  working GPU is recommended, not strictly required. See
  [Renderer selection](#7-renderer-selection--software-fallback).
- **Windowed mode** requires X or XWayland. A native Wayland backend is not yet
  implemented; it runs fine under XWayland.

### Video decode

- **H.264:** hardware VA-API (`vah264dec`) on Intel iGPUs.
- **H.265/HEVC:** no hardware decode on the tested Intel generations
  (Haswell / Sandy Bridge) → software `avdec_h265` (light: ~5–18% of a modern i7).
  Hardware HEVC is used automatically if the GPU + VA driver support it.
- Requires GStreamer with the VA-API + libav plugins (see [Build](#2-build)).

### Radio (Wi-Fi injection) NIC

- **Documented/tested here:** Realtek **RTL88xxAU family** — RTL8812AU / 8814AU /
  8811AU / 8821AU (e.g. Alfa AWUS036ACH). These need the **aircrack-ng out-of-tree
  driver for injection**: the in-kernel `rtw88` driver does monitor-mode RX but
  silently drops injected TX. See [Radio driver](#3-radio-driver-rtl88xxau-via-dkms).
- RubyFPV also supports `ath9k_htc` and MediaTek `mt76xx` adapters in general, but
  **this port's driver/DKMS instructions currently cover only the RTL88xxAU family.**
  Other chipsets require their own driver setup (not documented here yet).

---

## 2. Build

Install dependencies (Debian/Ubuntu names):

```bash
sudo apt install build-essential git bc libelf-dev \
  libpcap-dev \
  libcairo2-dev libdrm-dev libegl-dev libgles-dev libgbm-dev libx11-dev libxext-dev \
  libfreetype-dev libfontconfig1-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-vaapi gstreamer1.0-libav
# VA-API driver: i965-va-driver (Haswell/older) OR intel-media-va-driver (newer); plus vainfo to verify
sudo apt install i965-va-driver vainfo
```

Build the ground-station binaries:

```bash
make station ruby_central RUBY_BUILD_ENV=x64 -j"$(nproc)"
```

By default binaries install/run under `/opt/rubyfpv` (compile-time `RUBY_PREFIX`,
overridable with `-DRUBY_PREFIX='"/your/path"'`).

---

## 3. Radio driver (RTL88xxAU) via DKMS

RubyFPV injects raw 802.11 frames, which the in-kernel `rtw88` driver does not do.
Use the **aircrack-ng `rtl8812au`** driver, installed via **DKMS** so it is
**automatically rebuilt on every kernel upgrade** (a plain `make install` puts the
module only under the *current* kernel — a kernel bump then leaves the dongle
driverless: no `wlan0`, "no radio").

1. Get the driver source and blacklist the in-kernel driver:
   ```bash
   git clone https://github.com/aircrack-ng/rtl8812au.git ~/rtl8812au
   echo -e "blacklist rtw88_8812au\nblacklist rtw88_8812a" | \
     sudo tee /etc/modprobe.d/blacklist-rtw88-8812au.conf
   ```
   On very recent kernels the driver may need small compat fixes (e.g. timer API
   renames, `ccflags-y`); apply your port before installing.

2. Install with DKMS (the tree ships a `dkms.conf`):
   ```bash
   sudo cp -a ~/rtl8812au /usr/src/realtek-rtl88xxau-5.6.4.2~20230501
   sudo dkms add    -m realtek-rtl88xxau -v 5.6.4.2~20230501
   sudo dkms build  -m realtek-rtl88xxau -v 5.6.4.2~20230501
   sudo dkms install -m realtek-rtl88xxau -v 5.6.4.2~20230501
   ```

3. **Ensure the kernel *headers meta* package is installed** so future kernels
   arrive with headers and the DKMS post-install hook can rebuild automatically —
   e.g. `linux-headers-generic-hwe-<release>` (the sibling of the image meta
   package). Without it, `AUTOINSTALL` cannot rebuild after a kernel upgrade.

4. **Secure Boot:** if enabled, either disable it in firmware, or enrol a MOK so
   DKMS-signed modules load. (Apple Macs have no Secure Boot — nothing to do.)

> Only the RTL88xxAU family is covered by these instructions today. Contributions
> adding `ath9k_htc` / `mt76` setup are welcome.

---

## 4. Install & capabilities

- Place the built binaries + `res/` + launchers under `/opt/rubyfpv`.
- The radio/scheduling processes need capabilities. Grant them **after** `chown`
  (chown strips file caps):
  ```bash
  sudo chown -R root:root /opt/rubyfpv
  sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+eip \
    /opt/rubyfpv/ruby_rt_station /opt/rubyfpv/ruby_start \
    /opt/rubyfpv/ruby_tx_rc /opt/rubyfpv/ruby_rx_telemetry
  ```
- Keep the **binaries + launchers root-owned and not user-writable** — that is what
  makes the secured-sudo boundary (§5) safe.
- **Writable data dirs must be writable by whichever account runs the GS** —
  `config/`, `logs/`, `media/`, `tmp/`. The console kiosk runs as **root** (already
  fine), but a **windowed / non-root** run does not, so give those dirs a shared
  group + setgid so both root and the user can write and new files inherit the group:
  ```bash
  sudo groupadd -f rubyfpv
  sudo usermod -aG rubyfpv <gs-user>      # the desktop user, and/or the kiosk 'ruby' user
  sudo chown -R root:rubyfpv /opt/rubyfpv/config /opt/rubyfpv/logs /opt/rubyfpv/media /opt/rubyfpv/tmp
  sudo chmod -R g+w        /opt/rubyfpv/config /opt/rubyfpv/logs /opt/rubyfpv/media /opt/rubyfpv/tmp
  sudo find /opt/rubyfpv/config /opt/rubyfpv/logs /opt/rubyfpv/media /opt/rubyfpv/tmp -type d -exec chmod g+s {} \;
  ```
  Group membership takes effect on the next login. **Skipping this on a
  windowed/non-root install causes a "disk write errors" alarm** (the GS can't write
  `config/`/`logs/`).

Note: running from the physical console still needs root (radio `ip`/`iw`
subprocesses do not inherit file-caps, and first-boot may reboot); the launchers
are intended to be started with `sudo` — see the secured-sudo rule next.

### Redeploying after a rebuild

Copy the new binaries over, then **re-apply file capabilities** (chown/copy strips
them) and clear stale shared memory before restarting:

```bash
sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+eip \
  /opt/rubyfpv/ruby_rt_station /opt/rubyfpv/ruby_start \
  /opt/rubyfpv/ruby_tx_rc /opt/rubyfpv/ruby_rx_telemetry
sudo rm -f /dev/shm/RUBY* /dev/shm/SSMR* /dev/shm/SYSTEM* /dev/shm/R_* /dev/shm/sem.*
```

---

## 5. Secured sudo (via the `rubyfpv` group)

The GS needs root (radio + DRM), but operators should not have full sudo. Grant the
launchers to the **`rubyfpv` group** (§4) rather than a single user, so the console
kiosk account and any windowed/desktop user get them by group membership:

```bash
# /etc/sudoers.d/rubyfpv-gs   (mode 0440)
%rubyfpv ALL=(root) NOPASSWD: /opt/rubyfpv/ruby_console_start.sh, /opt/rubyfpv/ruby_stop_gs.sh
```

Install + validate, and add the GS accounts to the group:

```bash
sudo visudo -cf /etc/sudoers.d/rubyfpv-gs && sudo chmod 0440 /etc/sudoers.d/rubyfpv-gs
sudo usermod -aG rubyfpv <user>    # kiosk 'ruby' user and/or the desktop user; effective next login
```

The privilege boundary holds **only because the launcher scripts + binaries are
root-owned and not writable by group members** — otherwise a member could edit an
allowed script and escalate. Grant specific root-owned scripts, never a directory or
wildcard the group can write into (this is why §4 keeps the binaries/launchers
root-owned and only the *data dirs* group-writable).

**Windowed (non-root) note:** the console launcher runs wholly as root via the grant
above. The windowed launcher instead runs the GS as the desktop user and uses
`sudo -n` for a few prep steps (NetworkManager/`iw` radio setup, CPU-turbo toggle,
`/dev/shm` cleanup). On a locked-down windowed box, add those specific commands to
the `%rubyfpv` grant too (or route them through one small root-owned helper and grant
just that); on a dev box the desktop user usually already has broader sudo.

---

## 6. Kiosk mode (auto-start on boot)

### Console kiosk (recommended)

1. Boot to multi-user (no desktop grabbing DRM):
   ```bash
   sudo systemctl set-default multi-user.target
   ```

2. Auto-login the `ruby` user on tty1:
   ```ini
   # /etc/systemd/system/getty@tty1.service.d/autologin.conf
   [Service]
   ExecStart=
   ExecStart=-/sbin/agetty --autologin ruby --noclear %I $TERM
   ```

3. Launch the GS from the login shell (and log out cleanly on exit):
   ```bash
   # ~ruby/.bash_profile
   if [ "$(tty)" = "/dev/tty1" ]; then
     exec sudo /opt/rubyfpv/ruby_console_start.sh
   fi
   ```

4. **Audio:** the GS plays direct ALSA (`plughw`), so the kiosk user needs no sound
   server. Mask PipeWire/WirePlumber for `ruby` so it does not fight over the
   hardware mixer:
   ```bash
   sudo -u ruby mkdir -p ~ruby/.config/systemd/user
   for s in pipewire.service pipewire.socket pipewire-pulse.service \
            pipewire-pulse.socket wireplumber.service; do
     sudo -u ruby ln -sf /dev/null ~ruby/.config/systemd/user/$s
   done
   ```

### Windowed kiosk (X/XWayland)

Auto-login to a minimal session that runs `/opt/rubyfpv/ruby_window_start.sh` (e.g.
from `~/.xinitrc` / the compositor's autostart). The window supports **F11**
fullscreen. The same secured-sudo rule applies (add `ruby_window_start.sh` to the
sudoers line if the launcher needs root on your setup).

For a desktop (GNOME etc.) launcher entry with the RubyFPV icon, install a
`/usr/share/applications/rubyfpv.desktop` and run `update-desktop-database`:

```ini
[Desktop Entry]
Type=Application
Name=RubyFPV Ground Station
Comment=RubyFPV digital FPV ground station (windowed)
Exec=/opt/rubyfpv/ruby_window_start.sh
Icon=/opt/rubyfpv/res/ruby_logo.png
Terminal=false
Categories=AudioVideo;Network;Utility;
StartupWMClass=RubyFPV
```

`StartupWMClass=RubyFPV` matches the window so the taskbar shows the RubyFPV icon and
name (not a generic one). The launcher runs as the desktop user, so the data dirs
must be writable by that user (see §4).

---

## 7. Renderer selection & software fallback

The GPU compositor is the default in both modes and auto-falls-back on any init
failure. You can force the software path per mode:

| Mode | GPU path (default) | Force software fallback |
|---|---|---|
| Console (DRM) | EGL-on-GBM + GLES2, DRM page-flip | `RUBY_DRM_GL=0`, or `touch /opt/rubyfpv/config/no_drm_gl` |
| Windowed (X) | EGL/GLES2 on X | `RUBY_WINDOW_NOGL=1` |

The software fallback (CPU cairo compositor) is correct and complete — it is just
more CPU/heat. Use it to diagnose GPU/driver issues, or on hardware without a
usable GL stack.

### Environment knobs

| Variable | Effect |
|---|---|
| `RUBY_DRI_CARD` | DRM card to use (e.g. `/dev/dri/card1`). Launcher auto-picks the connected i915 card. |
| `RUBY_WLAN_IFACE` | Radio NIC name. Launcher auto-detects by chipset. |
| `RUBY_WINDOW_W` / `RUBY_WINDOW_H` | Internal render resolution (GPU upscales to the panel). Lower = less CPU/heat. |
| `RUBY_WINDOW_FULLSCREEN` | Start the window fullscreen (F11 toggles at runtime). |
| `RUBY_WINDOW_VSYNC` | `1` forces app-side vsync (default off; the compositor syncs). |
| `RUBY_DRM_GL` / `RUBY_WINDOW_NOGL` | Renderer selection (see table above). |
| `RUBY_MAXLAG_MS` | Software-decoder backlog cap in ms (default 200). Bounds live latency. |
| `RUBY_AUDIO_DEV` | Override the auto-detected ALSA playback card. |
| `RUBY_NO_TURBO_CAP` | `1` disables the launcher's CPU-turbo cap (see Thermals). |

---

## 8. Audio

Live FPV audio + notification sounds play via **direct ALSA** (`plughw`), not a sound
server. The analog (non-HDMI) playback card is auto-detected by name; override with
`RUBY_AUDIO_DEV`.

- **Vehicle-side prerequisite:** the GS only plays audio if the **vehicle** has an
  audio device enabled (model `audio_params.has_audio_device` + enabled; OpenIPC also
  needs the mic flag). No audio on the vehicle → silence on the GS, by design.
- **Kiosk:** mask PipeWire/WirePlumber for the kiosk user (see §6) so it doesn't grab
  the hardware mixer. RubyFPV does not change the system volume on x64 except via the
  volume keys.
- **Volume keys** (multimedia mute / down / up) adjust the detected card's mixer.

## 9. RC / joystick input

Keyboard and USB joystick/gamepad input use Linux `evdev`. The user running the GS
must be able to read `/dev/input/*`:

- Root (the console kiosk) already has access.
- A **non-root** user needs the `input` group (`sudo usermod -aG input <user>`), plus
  the `video` group (`sudo usermod -aG video <user>`) for DRM `/dev/dri/*` access when
  not on the active seat (e.g. started over SSH).

## 10. Recording (DVR)

- Recordings are written **straight to disk** (SSD) on x64 — no fixed-size RAM-disk
  cache and no "disk full → auto-stop" fixed-length clips (those are kept for
  Pi/Radxa to protect their SD cards).
- Files land in the media folder named by **wall-clock start time**:
  `video-<vehicle>-YYYY.MM.DD.HH.MM.SS`, with matching `.info` / `.h264`|`.h265` /
  `.osd` / `.srt`. This needs a correct system clock (RTC/NTP).
- The stored video is a **raw H.264/H.265 elementary stream**. Play it with the in-app
  DVR, or remux for external players:
  `ffmpeg -framerate <fps> -i in.h264 -c:v copy out.mp4`.

## 11. Thermals (thermally-marginal laptops)

Biggest lever first:

1. **GPU compositing** (default) removes the CPU video→OSD blit — the main heat
   source in windowed mode.
2. **CPU turbo cap while running** — the launchers write `1` to
   `/sys/devices/system/cpu/intel_pstate/no_turbo` on start and restore `0` on exit
   (opt out with `RUBY_NO_TURBO_CAP=1`). The GS at 1080p60 (HW decode + GPU
   composite) never saturates a core, so base clock suffices and dropping turbo
   voltage cuts ~10 °C.
3. **Apple laptops:** install a fan daemon (`mbpfan`) — the SMC otherwise under-runs
   the fans under this light-but-sustained load. On old machines, fresh thermal
   paste can be the difference between throttling and not.

---

## 12. Known limitations & operational notes

- **Use LEGACY OFDM datarates, not MCS, with RTL8812AU.** MCS rates can trigger an
  rtl8812au RX-FIFO/MAC lockup (video stalls; recovery = radio module reload).
  Legacy OFDM avoids it. (Best datarate is RF-environment dependent.)
- **Regulatory domain resets to world (`00`) each boot**, which blocks some 5 GHz
  DFS channels; the launcher runs `iw reg set US`. Adjust for your region.
- **Keep NetworkManager off the radio NIC** — NM/wpa_supplicant scan it off-channel
  and break the link. The launcher unmanages the detected NIC; persist it with a
  `NetworkManager/conf.d` `unmanaged-devices` entry.
- **No hardware HEVC** on the tested Intel generations (software `avdec_h265`).
- **No native Wayland backend** yet (runs under XWayland).
- **Interface naming:** RubyFPV accepts `wlan*` or `wlx*`; a udev `.link` rule
  matching by `Driver=rtl88XXau` gives a stable name across dongle swaps.

---

## 13. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| **"No radio" / no `wlan0`** | Driver not built for the running kernel — rebuild via DKMS (§3); or the NIC is grabbed by NetworkManager — unmanage it (§12). |
| **Black live video, OSD renders** | Missing/mismatched decoder — check `vainfo` and the GStreamer VA/libav plugins; confirm the vehicle codec (H.264 = HW, H.265 = software). |
| **Video stalls after a while (RTL8812AU)** | MCS-rate RX-FIFO lockup — switch the link to legacy OFDM; recover with a radio module reload. |
| **`/dev/dri/card*` open fails** | Not on the active seat (SSH) — add the user to `video`, or run from the console; ensure `multi-user.target` so no desktop holds DRM. |
| **Console blanks / garbled output** | Isolate GL with the software renderer (`RUBY_DRM_GL=0`); confirm the panel is on an atomic-KMS GPU (§1 + appendix). |
| **Audio silent** | Enable audio on the vehicle (§8); check the card with `RUBY_AUDIO_DEV`; ensure PipeWire is masked for the kiosk user. |
| **"Disk write errors" alarm / `Permission denied` on shared memory** | The GS account can't write `/opt/rubyfpv/{config,logs}` — on a windowed/non-root run, set the `rubyfpv` group + setgid on the data dirs (§4). Or stale root-owned `/dev/shm/RUBY*` from a prior console (root) session that was killed instead of exited cleanly — clear it: `sudo rm -f /dev/shm/RUBY* /dev/shm/SSMR* /dev/shm/SYSTEM* /dev/shm/R_* /dev/shm/sem.*`. |

## Appendix: dual-GPU MacBook display routing

On dual-GPU MacBooks (Intel iGPU + NVIDIA dGPU) the internal panel is wired to the
NVIDIA GPU, whose `nouveau` driver is legacy-KMS only — the atomic-KMS console
renderer cannot use it. Route the panel to the Intel GPU:

1. **Enable the Intel IGD** — kernel 6.11+ has an in-kernel Apple `set_os` quirk for
   supported models (do **not** hand-build `apple_set_os.efi`).
2. **Flip the gmux** to the integrated GPU (e.g. `gpu-switch -i`).
3. **Blacklist nouveau** and force i915:
   ```
   # /etc/modprobe.d/blacklist-nouveau.conf
   blacklist nouveau
   # GRUB cmdline:
   i915.modeset=1 nouveau.modeset=0 modprobe.blacklist=nouveau
   ```
   then `update-grub && update-initramfs -u`.

Single-GPU laptops need none of this.
