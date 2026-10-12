#!/usr/bin/env bash
# Steam Frame: runs frame-xrprobe (Outbreak VR, milestone M0) and logs everything.
#
# It sits at the top of the extracted outbreak-vr-frame-<sha> folder (the AppDir). Run it from there over
# SSH, or make it the devkit title's Start Command. Logs go to ~/frame-probe:
#   probe-<time>.log   the probe's own log (the answers)
#   launch-<time>.log  this script's notes, plus everything the probe, the OpenXR loader and the runtime
#                      print. The Frame keeps no stdout, so nothing here relies on a terminal.
#
# Settings (Steam launch options, e.g. "FRAME_NO_FDM=1 %command% --seconds-per-phase 12", or before the
# command over SSH):
#   FRAME_NO_FDM=1         disable Valve's FDM injection layer (it can hang vkCreateDevice, SteamVR-for-Linux #972)
#   FRAME_NO_GSWSI=1       disable the gamescope WSI layer
#   FRAME_PROBE_LOG_DIR=D  log somewhere other than ~/frame-probe
# Arguments go on to frame-xrprobe: --no-session, --seconds-per-phase N.

set -u

HERE=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
LOG_DIR=${FRAME_PROBE_LOG_DIR:-$HOME/frame-probe}
mkdir -p "$LOG_DIR"
STAMP=$(date +%Y%m%d-%H%M%S)
LAUNCH_LOG="$LOG_DIR/launch-$STAMP.log"

# Into the launch log; through tee as well when someone is watching a terminal (SSH).
if [ -t 1 ]; then
	exec > >(tee -a "$LAUNCH_LOG") 2>&1
else
	exec >>"$LAUNCH_LOG" 2>&1
fi

log() {
	echo "[frame-probe.sh $(date +%H:%M:%S)] $*"
}

log "Started $(date '+%Y-%m-%d %H:%M:%S %z') from $HERE"
log "uname -a: $(uname -a)"
log "Page size: $(getconf PAGESIZE)"
log "ldd: $(ldd --version 2>&1 | head -n 1)"
log "id: $(id)"
if [ -r /etc/os-release ]; then
	log "/etc/os-release:"
	sed 's/^/    /' /etc/os-release
fi
# Valve's docs name the Sniper runtime for devkit titles; the community saw arm64 titles run on the host.
if [ -e /run/host/container-manager ]; then
	log "Container: $(cat /run/host/container-manager)"
elif [ -e /run/host ]; then
	log "Container: /run/host exists, so probably some container"
else
	log "Container: none found (no /run/host), so this runs on the host system"
fi

# SteamVR's sockets live under XDG_RUNTIME_DIR, which an SSH login may not set.
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [ "${FRAME_NO_FDM:-0}" = "1" ]; then
	export DISABLE_VULKAN_FDM_INJECTION_LAYER=1
	log "FRAME_NO_FDM=1: DISABLE_VULKAN_FDM_INJECTION_LAYER=1"
fi
if [ "${FRAME_NO_GSWSI:-0}" = "1" ]; then
	export DISABLE_GAMESCOPE_WSI=1
	log "FRAME_NO_GSWSI=1: DISABLE_GAMESCOPE_WSI=1"
fi
# The OpenXR loader's own messages say which runtime manifest and which API layers it picked.
export XR_LOADER_DEBUG=${XR_LOADER_DEBUG:-info}

log "Environment:"
env | grep -E '^(XDG_|XR_|STEAM|DISABLE_|ENABLE_|VK_|MESA_|PRESSURE_VESSEL|WAYLAND_DISPLAY=|DISPLAY=|LD_LIBRARY_PATH=|LD_PRELOAD=|FRAME_)' | sort | sed 's/^/    /'

PROBE="$HERE/usr/bin/frame-xrprobe"
if [ ! -e "$PROBE" ]; then
	log "frame-xrprobe is missing: $PROBE"
	exit 2
fi
# A copy made on Windows (for a devkit upload) can lose the executable bit.
if [ ! -x "$PROBE" ]; then
	log "frame-xrprobe was not executable; fixing that"
	chmod +x "$PROBE" || exit 2
fi

log "Running: $PROBE --log-dir $LOG_DIR $*"
"$PROBE" --log-dir "$LOG_DIR" "$@"
code=$?
log "frame-xrprobe exited with $code"
exit "$code"
