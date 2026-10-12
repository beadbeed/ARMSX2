#!/usr/bin/env bash
# Outbreak VR: headset-free checks of the Steam Frame bundle. outbreak-vr.yml runs
# this (job frame-smoke) on ubuntu-24.04-arm, which has the Frame image's glibc 2.39.
#   1. frame-xrprobe with no OpenXR runtime, with --no-session and without: exit 0,
#      page size 4096 and "runtime unavailable" in the log, and a Vulkan device
#      (lavapipe), because the probe lists the devices directly without a runtime.
#   2. frame-probe.sh runs the probe and writes the launch log and the probe log.
#   3. frame-xrprobe needs only glibc and libgcc_s, and no ELF in the AppDir needs a
#      GLIBC_ symbol version above 2.39.
#   4. bash -n passes on the scripts.
# Usage: frame-smoke.sh <directory holding outbreak-vr-frame-*.tar.zst>

set -euo pipefail

DL=${1:?usage: frame-smoke.sh <directory holding outbreak-vr-frame-*.tar.zst>}
MAX_GLIBC=2.39
fail=0

note() {
	echo "::error::$*"
	fail=1
}

archive=$(ls "$DL"/outbreak-vr-frame-*.tar.zst | head -n 1)
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/frame-smoke.XXXXXX")
tar -C "$work" --zstd -xf "$archive"
appdir=$(ls -d "$work"/outbreak-vr-frame-* | head -n 1)
probe="$appdir/usr/bin/frame-xrprobe"
echo "Bundle: $archive ($(du -h "$archive" | cut -f1)) -> $appdir"

echo "== Layout"
for f in AppRun usr/bin/armsx2-qt usr/bin/frame-xrprobe frame-probe.sh README-FRAME.md; do
	if [ -e "$appdir/$f" ]; then
		echo "ok   $f"
	else
		note "missing from the bundle: $f"
	fi
done
[ -x "$probe" ] || note "usr/bin/frame-xrprobe is not executable"
[ -x "$appdir/frame-probe.sh" ] || note "frame-probe.sh is not executable"

echo "== What frame-xrprobe needs"
# glibc and libgcc_s only: libstdc++ is linked statically and the OpenXR loader
# (with its jsoncpp) is built in, so the probe starts on whatever the Frame has.
needed=$(objdump -p "$probe" | awk '/NEEDED/ { print $2 }')
for lib in $needed; do
	echo "  $lib"
	case "$lib" in
		libc.so.6 | libm.so.6 | libgcc_s.so.1 | libdl.so.2 | libpthread.so.0 | ld-linux-aarch64.so.1) ;;
		*) note "frame-xrprobe needs $lib; only glibc and libgcc_s are expected" ;;
	esac
done

# No runtime anywhere the loader looks: XR_RUNTIME_JSON and the runner's XDG
# directories are unset, and HOME is an empty folder.
NO_RUNTIME_ENV=(env -u XR_RUNTIME_JSON -u XDG_CONFIG_HOME -u XDG_CONFIG_DIRS -u XDG_DATA_HOME -u XDG_DATA_DIRS)
run_probe() {
	local name=$1
	shift
	local dir="$work/$name"
	mkdir -p "$dir"
	echo "== frame-xrprobe $* (no OpenXR runtime)"
	set +e
	"${NO_RUNTIME_ENV[@]}" HOME="$work/home-$name" "$probe" --log-dir "$dir" "$@" 2>"$dir/stderr.txt"
	local code=$?
	set -e
	local log
	log=$(ls "$dir"/probe-*.log 2>/dev/null | head -n 1 || true)
	if [ -z "$log" ]; then
		note "$name: the probe wrote no log (exit code $code)"
		cat "$dir/stderr.txt"
		return
	fi
	cat "$log"
	echo "-- stderr lines that are not the probe's own (the OpenXR loader's messages):"
	grep -v '^\[' "$dir/stderr.txt" || true
	[ "$code" -eq 0 ] || note "$name: exit code $code, expected 0"
	grep -q 'Page size: 4096 bytes' "$log" || note "$name: the log lacks 'Page size: 4096 bytes'"
	grep -q 'runtime unavailable' "$log" || note "$name: the log lacks 'runtime unavailable'"
	grep -q 'Vulkan device 0:' "$log" || note "$name: the log lists no Vulkan device (lavapipe expected)"
	grep -q 'frame-xrprobe finished, exit code 0' "$log" || note "$name: the log lacks the finish line"
}
run_probe no-session --no-session
run_probe full

echo "== frame-probe.sh --no-session (no OpenXR runtime)"
home="$work/home-script"
mkdir -p "$home"
set +e
"${NO_RUNTIME_ENV[@]}" HOME="$home" "$appdir/frame-probe.sh" --no-session
code=$?
set -e
[ "$code" -eq 0 ] || note "frame-probe.sh: exit code $code, expected 0"
launch=$(ls "$home"/frame-probe/launch-*.log 2>/dev/null | head -n 1 || true)
probe_log=$(ls "$home"/frame-probe/probe-*.log 2>/dev/null | head -n 1 || true)
if [ -n "$launch" ]; then
	cat "$launch"
	grep -q 'frame-xrprobe exited with 0' "$launch" || note "frame-probe.sh: the launch log lacks 'frame-xrprobe exited with 0'"
else
	note "frame-probe.sh wrote no launch log"
fi
[ -n "$probe_log" ] || note "frame-probe.sh: the probe wrote no log in ~/frame-probe"

echo "== GLIBC symbol versions (the Frame image has glibc $MAX_GLIBC)"
highest=""
checked=0
while IFS= read -r -d '' f; do
	# ELF files only (magic 7f 45 4c 46).
	[ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ] || continue
	checked=$((checked + 1))
	vers=$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -uV || true)
	[ -n "$vers" ] || continue
	top=$(echo "$vers" | tail -n 1)
	if [ "$(printf '%s\n%s\n' "$top" "$MAX_GLIBC" | sort -V | tail -n 1)" != "$MAX_GLIBC" ]; then
		note "$(realpath --relative-to="$appdir" "$f") needs GLIBC_$top"
	fi
	highest=$(printf '%s\n%s\n' "$highest" "$top" | sed '/^$/d' | sort -V | tail -n 1)
	if [ "$f" = "$probe" ]; then
		echo "frame-xrprobe needs GLIBC_$top at most"
	fi
done < <(find "$appdir" -type f -print0)
echo "ELF files checked: $checked; the highest GLIBC_ version any of them needs: ${highest:-none}"
[ "$checked" -gt 0 ] || note "no ELF files found in the AppDir"

echo "== bash -n"
for s in frame/*.sh "$appdir/frame-probe.sh" .github/workflows/scripts/linux/frame-smoke.sh; do
	if bash -n "$s"; then
		echo "ok   $s"
	else
		note "bash -n failed: $s"
	fi
done

if [ "$fail" -ne 0 ]; then
	echo "Steam Frame smoke test: FAILED"
	exit 1
fi
echo "Steam Frame smoke test: passed"
