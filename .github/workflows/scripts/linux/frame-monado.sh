#!/usr/bin/env bash
# Outbreak VR: the Steam Frame bundle against a real OpenXR runtime, with no headset. outbreak-vr.yml runs this
# (job frame-monado) on ubuntu-24.04-arm against Monado (build-monado.sh): in-process, the simulated HMD
# (SIMULATED_ENABLE=1) and the null compositor (XRT_COMPOSITOR_NULL=1), with Mesa's lavapipe for Vulkan.
#   probe     frame-xrprobe --seconds-per-phase 3: an instance, a Vulkan device through XR_KHR_vulkan_enable2, a
#             session that reaches VISIBLE or FOCUSED, frames rendered in every colour phase (A-D) with no
#             xrEndFrame error, a clean teardown and exit code 0.
#   vr-info   QT_QPA_PLATFORM=offscreen AppRun -vr-info: ARMSX2's VR layer finds Monado and the simulated HMD.
#   session   AppRun --vr -bigpicture on Xvfb, with the launcher's first-run ini and no game: Big Picture opens the
#             GS without a VM, so the VR layer creates its Vulkan device through XR_KHR_vulkan_enable2 and its
#             session against Monado; SIGTERM then shuts ARMSX2 down and the VR layer tears down. No BIOS, disc,
#             memory card or savestate is ever used here.
# Usage: frame-monado.sh <directory holding outbreak-vr-frame-*.tar.zst> <Monado prefix> <log directory> probe|vr-info|session

set -euo pipefail

DL=${1:?usage: frame-monado.sh <bundle dir> <Monado prefix> <log dir> probe|vr-info|session}
MONADO=${2:?Monado prefix}
OUT=${3:?log directory}
CHECK=${4:?probe, vr-info or session}
fail=0

note() {
	echo "::error::$CHECK: $*"
	fail=1
}

summary() {
	if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
		echo "$*" >>"$GITHUB_STEP_SUMMARY"
	fi
}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

manifest=$(find "$MONADO" -name 'openxr_monado.json' | head -n 1)
[ -n "$manifest" ] || {
	echo "::error::no openxr_monado.json under $MONADO"
	exit 1
}
echo "Monado: $(cat "$MONADO/monado-version.txt" 2>/dev/null || echo unknown); manifest $manifest"

# The bundle, extracted once and shared by the three checks.
if [ ! -d "$OUT/bundle" ]; then
	archive=$(ls "$DL"/outbreak-vr-frame-*.tar.zst | head -n 1)
	mkdir -p "$OUT/bundle"
	tar -C "$OUT/bundle" --zstd -xf "$archive"
fi
appdir=$(ls -d "$OUT"/bundle/outbreak-vr-frame-* | head -n 1)
probe="$appdir/usr/bin/frame-xrprobe"

# Monado only: XR_RUNTIME_JSON names it, the XDG directories are unset and each run gets an empty HOME.
MONADO_ENV=(env -u XDG_CONFIG_HOME -u XDG_CONFIG_DIRS -u XDG_DATA_HOME -u XDG_DATA_DIRS
	XR_RUNTIME_JSON="$manifest" XRT_COMPOSITOR_NULL=1 SIMULATED_ENABLE=1 XRT_LOG=debug)

want() {
	local file=$1 text=$2 what=$3
	grep -qF -- "$text" "$file" || note "$what (no '$text' in $(basename "$file"))"
}

case "$CHECK" in
probe)
	dir="$OUT/probe"
	mkdir -p "$dir" "$OUT/home-probe"
	echo "== Vulkan on this runner (what the null compositor needs from lavapipe)"
	vulkaninfo >"$OUT/vulkaninfo.txt" 2>&1 || true
	grep -E 'deviceName|driverName|driverInfo|apiVersion|VK_KHR_external_(memory|semaphore|fence)_fd|VK_KHR_timeline_semaphore' \
		"$OUT/vulkaninfo.txt" | sed 's/^[ \t]*//' | sort -u || true
	echo "== frame-xrprobe --seconds-per-phase 3 against Monado"
	set +e
	"${MONADO_ENV[@]}" HOME="$OUT/home-probe" timeout -k 10 300 "$probe" --seconds-per-phase 3 --log-dir "$dir" 2>"$dir/stderr.txt"
	code=$?
	set -e
	log=$(ls "$dir"/probe-*.log 2>/dev/null | head -n 1 || true)
	if [ -z "$log" ]; then
		note "the probe wrote no log (exit code $code)"
		head -n 400 "$dir/stderr.txt"
		exit 1
	fi
	cat "$log"
	echo "-- stderr lines that are not the probe's own (Monado's and the OpenXR loader's messages; all in the artifact):"
	grep -v '^\[' "$dir/stderr.txt" | head -n 400 || true
	[ "$code" -eq 0 ] || note "exit code $code, expected 0 (1: a call failed or the phases did not run; 3: the watchdog; 124: timeout)"

	want "$log" 'Runtime: Monado' "the runtime is not Monado"
	want "$log" 'Answers: XR_KHR_vulkan_enable2 YES' "XR_KHR_vulkan_enable2 is not offered"
	want "$log" 'xrCreateVulkanDeviceKHR: XR_SUCCESS' "no Vulkan device through XR_KHR_vulkan_enable2"
	want "$log" 'xrCreateSession (Vulkan' "no xrCreateSession"
	grep -q 'xrCreateSession (Vulkan.*: XR_SUCCESS' "$log" || note "xrCreateSession failed"
	grep -qE 'Session state: [A-Z_]+ -> (VISIBLE|FOCUSED) ' "$log" || note "the session never became VISIBLE or FOCUSED"
	reached=$(grep -oE 'Session state: [A-Z_]+ -> [A-Z_]+' "$log" | sed 's/.*-> //' | tr '\n' ' ')
	echo "Session states reached: $reached"

	phases=$(sed -n 's/.*Phases to run:\([A-D ]*\),.*/\1/p' "$log" | head -n 1 | xargs || true)
	expected="A B C"
	grep -q 'cylinder layer YES' "$log" && expected="A B C D"
	[ "$phases" = "$expected" ] || note "phases to run: '$phases', expected '$expected'"

	summary "### frame-xrprobe against Monado"
	summary ""
	summary "$(grep -m1 -F 'Runtime: ' "$log" | sed 's/^\[[^]]*\] //'); session states: $reached"
	summary ""
	summary "| Phase | Frames | Rendered | Display period | xrEndFrame errors |"
	summary "|---|---|---|---|---|"
	for p in $phases; do
		line=$(grep -m1 -F -- "---- Phase $p end:" "$log" || true)
		if [ -z "$line" ]; then
			note "phase $p never ended"
			summary "| $p | - | - | - | did not end |"
			continue
		fi
		frames=$(echo "$line" | sed -n 's/.*, \([0-9]*\) frames (\([0-9]*\) rendered).*/\1/p')
		rendered=$(echo "$line" | sed -n 's/.*, \([0-9]*\) frames (\([0-9]*\) rendered).*/\2/p')
		# The xrEndFrame and display period lines follow the phase's end line.
		errors=$(awk -v p="---- Phase $p end:" 'index($0, p) { f = 1; next } f && /xrEndFrame avg/ { if (match($0, /errors [0-9]+/)) print substr($0, RSTART + 7, RLENGTH - 7); exit }' "$log")
		period=$(awk -v p="---- Phase $p end:" 'index($0, p) { f = 1; next } f && /predictedDisplayPeriod/ { sub(/.*max /, ""); print; exit }' "$log")
		echo "Phase $p: $frames frames, $rendered rendered, xrEndFrame errors ${errors:-?}, period min/avg/max $period"
		summary "| $p | $frames | $rendered | ${period:-?} | ${errors:-?} |"
		[ "${rendered:-0}" -gt 0 ] || note "phase $p rendered no frame"
		[ "${errors:-x}" = 0 ] || note "phase $p: xrEndFrame errors ${errors:-unknown}"
	done
	grep -q 'was cut short' "$log" && note "a phase was cut short"
	want "$log" 'All phases done.' "the phases did not all run"
	want "$log" 'xrDestroySession: XR_SUCCESS' "xrDestroySession failed"
	want "$log" 'xrDestroyInstance: XR_SUCCESS' "xrDestroyInstance failed"
	want "$log" 'frame-xrprobe finished, exit code 0' "the log lacks the finish line with exit code 0"
	;;

vr-info)
	home="$OUT/home-vrinfo"
	mkdir -p "$home"
	echo "== armsx2-qt -vr-info against Monado (QT_QPA_PLATFORM=offscreen)"
	set +e
	"${MONADO_ENV[@]}" HOME="$home" QT_QPA_PLATFORM=offscreen timeout -k 10 120 "$appdir/AppRun" -vr-info >"$OUT/vr-info.txt" 2>&1
	code=$?
	set -e
	cat "$OUT/vr-info.txt"
	# -vr-info returns from the argument parser, so ARMSX2 exits 1, as with -version.
	case $code in
		0 | 1) echo "exit code $code" ;;
		*) note "exit code $code (124: hung, 127: did not start, 134 or 139: crashed)" ;;
	esac
	grep -q '^ARMSX2 ' "$OUT/vr-info.txt" || note "no version line"
	want "$OUT/vr-info.txt" 'Runtime: Monado' "ARMSX2's VR layer did not create an instance on Monado"
	want "$OUT/vr-info.txt" 'Headset: ' "no headset (simulated HMD) reported"
	want "$OUT/vr-info.txt" 'XR_KHR_vulkan_enable2' "XR_KHR_vulkan_enable2 is not in the extension list"
	;;

session)
	home="$OUT/home-session"
	mkdir -p "$home"
	echo "== launch-outbreak-vr.sh with no game: the first-run ini"
	set +e
	# MONADO_ENV unsets XDG_CONFIG_HOME (the runner sets it), so the ini lands under this HOME.
	"${MONADO_ENV[@]}" HOME="$home" "$appdir/launch-outbreak-vr.sh" >/dev/null 2>&1
	set -e
	ini="$home/.config/ARMSX2/inis/PCSX2.ini"
	[ -f "$ini" ] || {
		note "the launcher wrote no ini"
		exit 1
	}
	# The log to standard output as well as emulog.txt.
	printf '\n[Logging]\nEnableSystemConsole = true\nEnableFileLogging = true\n' >>"$ini"
	cat "$ini"

	echo "== Xvfb"
	display=:99
	Xvfb "$display" -screen 0 1280x720x24 -nolisten tcp >"$OUT/xvfb.txt" 2>&1 &
	xvfb_pid=$!
	for _ in $(seq 1 50); do
		[ -S /tmp/.X11-unix/X99 ] && break
		sleep 0.2
	done
	[ -S /tmp/.X11-unix/X99 ] || {
		note "Xvfb did not start"
		cat "$OUT/xvfb.txt"
		exit 1
	}

	echo "== AppRun --vr -bigpicture (no game) against Monado"
	out="$OUT/session.txt"
	emulog="$home/.config/ARMSX2/logs/emulog.txt"
	"${MONADO_ENV[@]}" HOME="$home" DISPLAY="$display" QT_QPA_PLATFORM=xcb "$appdir/AppRun" --vr -bigpicture >"$out" 2>&1 &
	pid=$!
	seen() { grep -qE -- "$1" "$out" "$emulog" 2>/dev/null; }
	# Up to 4 minutes for the GS to start on lavapipe and the session to be shown, then 10 s of frames.
	started=0
	for _ in $(seq 1 240); do
		if ! kill -0 "$pid" 2>/dev/null; then
			break
		fi
		if seen '\(VR\) VR session active' && seen '\(VR\) Session state: [A-Z_]+ -> (VISIBLE|FOCUSED)'; then
			started=1
			break
		fi
		sleep 1
	done
	if [ "$started" -eq 1 ]; then
		echo "Session shown; 10 s of frames."
		sleep 10
	else
		note "no VR session shown within 4 minutes (or ARMSX2 exited)"
	fi
	stopped=0
	if kill -0 "$pid" 2>/dev/null; then
		echo "SIGTERM (ARMSX2's graceful shutdown)"
		kill -TERM "$pid" || true
		for _ in $(seq 1 60); do
			kill -0 "$pid" 2>/dev/null || {
				stopped=1
				break
			}
			sleep 1
		done
		if [ "$stopped" -eq 0 ]; then
			note "ARMSX2 did not exit within 60 s of SIGTERM; killed"
			kill -KILL "$pid" 2>/dev/null || true
		fi
	fi
	set +e
	wait "$pid"
	code=$?
	set -e
	kill "$xvfb_pid" 2>/dev/null || true
	echo "-- ARMSX2's output (the first 1500 lines; all of it in the artifact):"
	head -n 1500 "$out"
	if [ -f "$emulog" ]; then
		cp "$emulog" "$OUT/emulog.txt"
		echo "-- emulog.txt, the (VR) lines:"
		grep -F '(VR)' "$emulog" || true
	fi
	[ "$code" -eq 0 ] || note "exit code $code, expected 0"
	logs=("$out")
	[ -f "$emulog" ] && logs+=("$emulog")
	check() {
		grep -qE -- "$1" "${logs[@]}" || note "$2"
	}
	check '\(VR\) OpenXR runtime: Monado' "the VR layer did not create an instance on Monado"
	check '\(VR\) HMD: ' "no HMD system"
	check '\(VR\) XR session created \(Vulkan' "no XR session (Vulkan device through XR_KHR_vulkan_enable2)"
	check '\(VR\) Session began' "the session never began"
	check '\(VR\) Session state: [A-Z_]+ -> (VISIBLE|FOCUSED)' "the session was never VISIBLE or FOCUSED"
	check '\(VR\) VR session active' "no 'VR session active'"
	check '\(VR\) GS device destroyed' "the VR layer was not torn down with the GS device"
	check '\(VR\) XR session destroyed' "the XR session was not destroyed"
	if grep -qE '\(VR\).*(abandon|queue lock held over)' "${logs[@]}"; then
		note "the VR layer abandoned VR or held the queue lock too long"
	fi
	summary "### ARMSX2 --vr -bigpicture against Monado (no game)"
	summary ""
	summary "Exit code $code. Session states: $(grep -hoE '\(VR\) Session state: [A-Z_]+ -> [A-Z_]+' "$out" | sed 's/.*-> //' | tr '\n' ' ')"
	;;

*)
	echo "unknown check '$CHECK': probe, vr-info or session" >&2
	exit 2
	;;
esac

if [ "$fail" -ne 0 ]; then
	echo "frame-monado $CHECK: FAILED"
	exit 1
fi
echo "frame-monado $CHECK: passed"
