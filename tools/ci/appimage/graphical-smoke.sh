#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

if [[ $# != 3 || ! $2 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ || ! $3 =~ ^/tmp/[A-Za-z0-9_.-]+$ ]]; then
  echo "usage: $0 PACKAGE.AppImage VERSION /tmp/ABSENT-EVIDENCE-DIRECTORY" >&2
  exit 2
fi
[[ ${ATRINIK_APPIMAGE_SMOKE_CONTAINER:-} == 1 && $EUID != 0 ]]
probe=${ATRINIK_APPIMAGE_GRAPHICAL_PROBE:?trusted separate graphical probe is required}
[[ -f $probe && ! -L $probe && -x $probe ]]
package=$(realpath -e "$1")
version=$2
evidence=$3
[[ ! -e $evidence && ! -L $evidence ]]
umask 077
mkdir "$evidence"
evidence=$(realpath -e "$evidence")
[[ $evidence == /tmp/* ]]
# Host services/drivers are allowed. SDL and cURL must come from the AppImage.
if ldconfig -p | grep -E 'libSDL3|libcares|libcurl'; then
  echo 'graphical host unexpectedly contains application libraries' >&2
  exit 1
fi
work=$(mktemp -d)
x_pid='' pulse_pid='' probe_pid=''
finish() {
  local status=$?
  trap - EXIT
  for pid in "$probe_pid" "$pulse_pid" "$x_pid"; do
    if [[ -n $pid ]]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  if (( status != 0 )); then
    find "$evidence" -maxdepth 1 -name '*.log' -type f -exec tail -n 100 {} + >&2
  fi
  # Keep evidence in the container for a caller-owned docker cp on either result.
  exit "$status"
}
trap finish EXIT
mkdir "$work/extract" "$work/unrelated working directory" "$work/home" "$work/runtime"
export HOME="$work/home" XDG_RUNTIME_DIR="$work/runtime"
export DISPLAY=:99 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=pulseaudio
export PULSE_SERVER="unix:$work/runtime/pulse-native" PULSE_SINK=appimage_smoke
export LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2
# No host GPU device is exposed and only this CPU ICD is selectable.
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
[[ -f $VK_ICD_FILENAMES ]]
Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp -noreset > "$evidence/xvfb.log" 2>&1 &
x_pid=$!
pulseaudio -n --daemonize=no --exit-idle-time=-1 --log-target=stderr \
  --load="module-native-protocol-unix socket=$work/runtime/pulse-native auth-anonymous=1" \
  --load='module-null-sink sink_name=appimage_smoke rate=48000 channels=2' \
  > "$evidence/pulse.log" 2>&1 &
pulse_pid=$!
services_ready=0
for ((attempt = 0; attempt < 100; attempt++)); do
  kill -0 "$x_pid" && kill -0 "$pulse_pid"
  if xdpyinfo > "$evidence/display.log" 2>&1 && \
      pactl list short sinks > "$evidence/sinks.log" 2>&1 && \
      grep -F 'appimage_smoke' "$evidence/sinks.log" > /dev/null; then
    services_ready=1
    break
  fi
  sleep 0.1
done
[[ $services_ready == 1 ]]

cd "$work/extract"
# Runtime execution happens only after the caller's independent package validator.
timeout 30 "$package" --appimage-extract > "$evidence/extraction.log" 2>&1
mv squashfs-root "$work/relocated graphical Atrinik.AppDir"
appdir="$work/relocated graphical Atrinik.AppDir"
chmod -R a+rX,a-w "$appdir"
cd "$work/unrelated working directory"
# Production help exercises its actual relocated launcher and state paths. This
# is deliberately reported separately from the helper's software presentation.
env ATRINIK_CONFIG_DIR='persistent configuration with spaces' \
  timeout 30 "$appdir/AppRun" --help > "$evidence/client-help.log" 2>&1
grep -F 'List of available options' "$evidence/client-help.log"
state="$PWD/persistent configuration with spaces/.atrinik/${version%%.*}.x"
[[ -d $state ]]
printf '# graphical qualification persistence sentinel\n' > "$state/client-custom.cfg"
mv "$appdir" "$work/second relocation with spaces.AppDir"
appdir="$work/second relocation with spaces.AppDir"
env ATRINIK_CONFIG_DIR='persistent configuration with spaces' \
  timeout 30 "$appdir/AppRun" --help > "$evidence/client-persistent.log" 2>&1
grep -F "Loading configuration from $state/client-custom.cfg" "$evidence/client-persistent.log"
grep -Fx '# graphical qualification persistence sentinel' "$state/client-custom.cfg"

export LD_LIBRARY_PATH="$appdir/usr/lib"
timeout 45 "$probe" "$appdir" > "$evidence/probe.log" 2>&1 &
probe_pid=$!
graphical_ready=0
for ((attempt = 0; attempt < 300; attempt++)); do
  if grep -F 'graphical-ready backend=vulkan device=' "$evidence/probe.log" > /dev/null; then
    graphical_ready=1
    break
  fi
  if ! kill -0 "$probe_pid" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
[[ $graphical_ready == 1 ]]
# These host utilities must not load the AppImage's private application closure.
window=$(env -u LD_LIBRARY_PATH xdotool search --onlyvisible \
  --name '^Atrinik AppImage dependency qualification$')
[[ $window =~ ^[0-9]+$ ]]
env -u LD_LIBRARY_PATH xwininfo -id "$window" > "$evidence/window.log"
grep -F 'Width: 800' "$evidence/window.log"
grep -F 'Height: 600' "$evidence/window.log"
env -u LD_LIBRARY_PATH xwd -silent -id "$window" -out "$evidence/software-vulkan-frame.xwd"
[[ -s $evidence/software-vulkan-frame.xwd ]]
env -u LD_LIBRARY_PATH pactl list sink-inputs > "$evidence/audio-inputs.log"
grep -F 'Sink Input #' "$evidence/audio-inputs.log"
wait "$probe_pid"
probe_pid=
grep -F 'graphical-complete:' "$evidence/probe.log"
cp /graphical-host-packages.tsv "$evidence/host-packages.tsv"
printf '{"version":"%s","software_vulkan_dependency_presentation":true,"gpu_readback":true,"bundled_png_font":true,"virtual_audio_device":true,"launcher_relocation":true,"configuration_marker_persistence":true,"production_graphical_startup":false,"hardware_gameplay":false,"audible_playback":false}\n' \
  "$version" | tee "$evidence/result.json"
