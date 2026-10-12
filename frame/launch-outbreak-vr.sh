#!/usr/bin/env bash
# Steam Frame: starts Outbreak VR (ARMSX2 with --vr) and logs everything. Milestone M1: the game in first
# person, the head turning the game camera, both eyes rendered (stereo depth), the Frame controllers as the
# pad. FRAME_VIEW=flat gives M0's fixed flat screen instead.
#
# It sits at the top of the extracted outbreak-vr-frame-<sha> folder (the AppDir). Run it from there over
# SSH, or make it the devkit title's Start Command. Logs go to ~/armsx2-vr/logs/launch-<time>.log: this
# script's notes plus everything ARMSX2, the OpenXR loader and the runtime print. The Frame keeps no
# stdout, so nothing here relies on a terminal. ARMSX2's own log (emulog.txt) is in ~/.config/ARMSX2/logs.
#
# The game: the first argument, or else the first line of ~/armsx2-vr/game.txt. The BIOS goes in
# ~/.config/ARMSX2/bios. Neither ever comes with this folder: copy them onto the Frame yourself.
#
# Settings (Steam launch options, e.g. "FRAME_NO_FDM=1 %command%", or before the command over SSH):
#   FRAME_NO_FDM=1     disable Valve's FDM injection layer (it can hang vkCreateDevice, SteamVR-for-Linux #972)
#   FRAME_NO_GSWSI=1   disable the gamescope WSI layer
#   FRAME_VR_LOG_DIR=D log somewhere other than ~/armsx2-vr/logs
#   FRAME_VIEW=first-person   set [VR] HeadCamera and StereoMode to true in the ini (the M1 view), then start
#   FRAME_VIEW=flat           set them to false (M0's flat screen), then start
#   FRAME_PINE=1       set [EmuCore] EnablePINE = true in the ini (=0: false), for the research and test tools
#                      (tools/outbreak-vr/pine.py; PINE listens on $XDG_RUNTIME_DIR/pcsx2.sock while a game runs)
#
# First run: if ~/.config/ARMSX2/inis/PCSX2.ini does not exist, this writes a small one for the headset:
# no setup wizard, Vulkan at 1x, MTVU on, VR on with the first-person camera and stereo on (M1). Later
# runs leave the ini alone, so change it freely (or delete it to get this one back). FRAME_VIEW changes
# only those two keys and FRAME_PINE only EnablePINE; both keep everything else.

set -u

HERE=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
VR_HOME="$HOME/armsx2-vr"
LOG_DIR=${FRAME_VR_LOG_DIR:-$VR_HOME/logs}
mkdir -p "$LOG_DIR"
STAMP=$(date +%Y%m%d-%H%M%S)
LAUNCH_LOG="$LOG_DIR/launch-$STAMP.log"

# Into the launch log; through tee as well when someone is watching a terminal (SSH). tee ignores Ctrl+C
# and a dropped SSH connection, so it keeps writing while ARMSX2 shuts down.
if [ -t 1 ]; then
	exec > >(trap '' INT TERM HUP; exec tee -a "$LAUNCH_LOG") 2>&1
else
	exec >>"$LAUNCH_LOG" 2>&1
fi

log() {
	echo "[launch-outbreak-vr.sh $(date +%H:%M:%S)] $*"
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
env | grep -E '^(XDG_|XR_|STEAM|DISABLE_|ENABLE_|VK_|MESA_|PRESSURE_VESSEL|WAYLAND_DISPLAY=|DISPLAY=|QT_|LD_LIBRARY_PATH=|LD_PRELOAD=|FRAME_)' | sort | sed 's/^/    /'

# ARMSX2's data folder: $XDG_CONFIG_HOME/ARMSX2, else ~/.config/ARMSX2 (as ARMSX2 itself decides).
if [ -n "${XDG_CONFIG_HOME:-}" ] && [ "${XDG_CONFIG_HOME#/}" != "$XDG_CONFIG_HOME" ]; then
	DATA_ROOT="$XDG_CONFIG_HOME/ARMSX2"
else
	DATA_ROOT="$HOME/.config/ARMSX2"
fi
INI="$DATA_ROOT/inis/PCSX2.ini"
BIOS_DIR="$DATA_ROOT/bios"

if [ ! -e "$INI" ]; then
	log "First run: writing $INI for the headset"
	mkdir -p "$DATA_ROOT/inis"
	# SettingsVersion must match ARMSX2's (VMManager.cpp SETTINGS_VERSION), or ARMSX2 treats the file as
	# unreadable and writes its defaults over it (and opens the setup wizard). Keys left out keep ARMSX2's
	# defaults: Pad1 is a DualShock 2, the BIOS folder is $DATA_ROOT/bios, and the VR profile's VR Gamepad
	# adds its bindings to Pad1 on top of the ini's. [VR] SnapTurn = false: the right stick turns the view smoothly
	# (SnapTurn = true for 30 degree steps). Seated play needs no key: the VR layer uses the LOCAL (seated) space.
	cat >"$INI" <<'EOF'
[UI]
SettingsVersion = 1
SetupWizardIncomplete = false
ConfirmShutdown = false
PauseOnFocusLoss = false

[AutoUpdater]
CheckAtStartup = false

[EmuCore/GS]
Renderer = 14
upscale_multiplier = 1

[EmuCore/Speedhacks]
vuThread = true

[VR]
Enable = true
HeadCamera = true
StereoMode = true
SnapTurn = false
HeadsetRateSubmit = true
EOF
else
	log "Settings: $INI (left as it is)"
fi

# FRAME_VIEW: the first-person view (head camera and stereo) or M0's flat screen, written into [VR].
set_vr_view() {
	local value=$1 tmp="$INI.view.$$"
	awk -v v="$value" '
		function add_missing() { if (in_vr) { if (!hc) print "HeadCamera = " v; if (!sm) print "StereoMode = " v } }
		/^\[/ { add_missing(); in_vr = ($0 == "[VR]"); if (in_vr) seen = 1; hc = 0; sm = 0; print; next }
		in_vr && /^HeadCamera[ \t]*=/ { print "HeadCamera = " v; hc = 1; next }
		in_vr && /^StereoMode[ \t]*=/ { print "StereoMode = " v; sm = 1; next }
		{ print }
		END { add_missing(); if (!seen) { print ""; print "[VR]"; print "HeadCamera = " v; print "StereoMode = " v } }
	' "$INI" >"$tmp" && mv "$tmp" "$INI"
}
case "${FRAME_VIEW:-}" in
	"") ;;
	first-person | firstperson | fp)
		set_vr_view true && log "FRAME_VIEW=$FRAME_VIEW: [VR] HeadCamera = true, StereoMode = true in $INI" ;;
	flat | m0)
		set_vr_view false && log "FRAME_VIEW=$FRAME_VIEW: [VR] HeadCamera = false, StereoMode = false in $INI" ;;
	*)
		log "FRAME_VIEW=$FRAME_VIEW is not 'first-person' or 'flat'; the ini is left as it is" ;;
esac
if [ -r "$INI" ]; then
	log "[VR] view in the ini: $(grep -E '^(HeadCamera|StereoMode)[ \t]*=' "$INI" | tr '\n' ' ')"
	if [ -z "${FRAME_VIEW:-}" ] && grep -qE '^HeadCamera[ \t]*=[ \t]*false' "$INI"; then
		log "This ini has M0's flat screen (HeadCamera = false). For first person, run once with FRAME_VIEW=first-person (or delete the ini)."
	fi
fi

# FRAME_PINE: PINE on or off, written into [EmuCore] (the section is added if the ini has none).
set_emucore_pine() {
	local value=$1 tmp="$INI.pine.$$"
	awk -v v="$value" '
		function add_missing() { if (in_core && !done) { print "EnablePINE = " v; done = 1 } }
		/^\[/ { add_missing(); in_core = ($0 == "[EmuCore]"); if (in_core) seen = 1; print; next }
		in_core && /^EnablePINE[ \t]*=/ { if (!done) print "EnablePINE = " v; done = 1; next }
		{ print }
		END { add_missing(); if (!seen) { print ""; print "[EmuCore]"; print "EnablePINE = " v } }
	' "$INI" >"$tmp" && mv "$tmp" "$INI"
}
case "${FRAME_PINE:-}" in
	"") ;;
	1 | on | true)
		set_emucore_pine true && log "FRAME_PINE=$FRAME_PINE: [EmuCore] EnablePINE = true in $INI" ;;
	0 | off | false)
		set_emucore_pine false && log "FRAME_PINE=$FRAME_PINE: [EmuCore] EnablePINE = false in $INI" ;;
	*)
		log "FRAME_PINE=$FRAME_PINE is not 1 or 0; the ini is left as it is" ;;
esac
if [ -r "$INI" ] && grep -qE '^EnablePINE[ \t]*=[ \t]*true' "$INI"; then
	log "PINE is on: while the game runs, tools/outbreak-vr/pine.py reaches it at ${XDG_RUNTIME_DIR:-/tmp}/pcsx2.sock (slot 28011)"
fi

# The game: $1, or the first line of ~/armsx2-vr/game.txt.
GAME=""
if [ $# -ge 1 ] && [ -n "$1" ]; then
	GAME=$1
	shift
elif [ -r "$VR_HOME/game.txt" ]; then
	GAME=$(head -n 1 "$VR_HOME/game.txt" | tr -d '\r')
fi
if [ -z "$GAME" ]; then
	log "No game given. Pass the disc image as the first argument, or put its full path on the first line of $VR_HOME/game.txt."
	exit 2
fi
if [ ! -r "$GAME" ]; then
	log "The game is not there (or not readable): $GAME"
	exit 2
fi
if [ -z "$(find "$BIOS_DIR" -maxdepth 1 -type f 2>/dev/null | head -n 1)" ]; then
	log "No BIOS found in $BIOS_DIR. Copy your PS2 BIOS there (over the LAN, USB-C or microSD; never through a cloud service)."
	exit 2
fi
log "Game: $GAME"
log "BIOS folder: $BIOS_DIR ($(find "$BIOS_DIR" -maxdepth 1 -type f | wc -l) file(s))"

APP="$HERE/usr/bin/armsx2-qt"
if [ -x "$HERE/AppRun" ]; then
	APP="$HERE/AppRun"
fi
if [ ! -e "$APP" ]; then
	log "ARMSX2 is missing: $APP"
	exit 2
fi
# A copy made on Windows (for a devkit upload) can lose the executable bit.
for f in "$HERE/AppRun" "$HERE/usr/bin/armsx2-qt" "$HERE/usr/bin/frame-xrprobe"; do
	if [ -e "$f" ] && [ ! -x "$f" ]; then
		log "$f was not executable; fixing that"
		chmod +x "$f" || exit 2
	fi
done

log "Running: $APP --vr -fullscreen -batch $* -- $GAME"
"$APP" --vr -fullscreen -batch "$@" -- "$GAME"
code=$?
log "ARMSX2 exited with $code"
exit "$code"
