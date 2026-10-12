#!/usr/bin/env bash
# Outbreak VR: headset-free checks of the Steam Frame bundle. outbreak-vr.yml runs
# this (job frame-smoke) on ubuntu-24.04-arm, which has the Frame image's glibc 2.39.
#   1. frame-xrprobe with no OpenXR runtime, with --no-session and without: exit 0,
#      page size 4096 and "runtime unavailable" in the log, and a Vulkan device
#      (lavapipe), because the probe lists the devices directly without a runtime.
#      Also with XR_RUNTIME_JSON naming a runtime library that does not exist, and
#      with stderr into a pipe nobody reads (as when Ctrl+C ends tee over SSH).
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
# Extra NAME=VALUE pairs for the next run_probe (env applies them after the -u list).
EXTRA_ENV=()
run_probe() {
	local name=$1
	shift
	local dir="$work/$name"
	mkdir -p "$dir"
	echo "== frame-xrprobe $* (no OpenXR runtime${EXTRA_ENV[*]:+; ${EXTRA_ENV[*]}})"
	set +e
	"${NO_RUNTIME_ENV[@]}" HOME="$work/home-$name" "${EXTRA_ENV[@]}" "$probe" --log-dir "$dir" "$@" 2>"$dir/stderr.txt"
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
	# The loader's compiled-in SYSCONFDIR (CMAKE_INSTALL_PREFIX /usr/local in CI) is on the probe's list too.
	grep -q '/usr/local/etc/openxr/1/active_runtime.json' "$log" || note "$name: the manifest list lacks the loader's SYSCONFDIR"
	grep -q 'Vulkan device 0:' "$log" || note "$name: the log lists no Vulkan device (lavapipe expected)"
	grep -q 'frame-xrprobe finished, exit code 0' "$log" || note "$name: the log lacks the finish line"
}
run_probe no-session --no-session
run_probe full

# A manifest whose runtime library is not there: the loader finds no runtime, the probe
# says so and still exits 0, and it marks the XR_RUNTIME_JSON file as the one in use.
fake="$work/fake-runtime"
mkdir -p "$fake"
cat >"$fake/active_runtime.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "runtime": { "name": "frame-smoke missing runtime", "library_path": "./libmissing-runtime.so" }
}
EOF
EXTRA_ENV=("XR_RUNTIME_JSON=$fake/active_runtime.json")
run_probe broken-manifest --no-session
EXTRA_ENV=()
log=$(ls "$work"/broken-manifest/probe-*.log 2>/dev/null | head -n 1 || true)
if [ -n "$log" ]; then
	grep -q 'XR_RUNTIME_JSON is set, so the loader uses only' "$log" || note "broken-manifest: the log does not name the XR_RUNTIME_JSON override"
	grep -q "$fake/active_runtime.json   <- the loader uses this one" "$log" || note "broken-manifest: the override is not marked as the manifest in use"
	grep -q 'libmissing-runtime.so: MISSING' "$log" || note "broken-manifest: the missing runtime library is not reported"
fi

# stderr into a pipe whose reader has gone, as when Ctrl+C or a dropped SSH connection
# ends tee in frame-probe.sh: the probe must not die of SIGPIPE, and its log must end.
echo "== frame-xrprobe --no-session with stderr into a pipe nobody reads"
dir="$work/closed-pipe"
mkdir -p "$dir"
set +e
"${NO_RUNTIME_ENV[@]}" HOME="$work/home-closed-pipe" "$probe" --no-session --log-dir "$dir" 2>&1 | true
code=${PIPESTATUS[0]}
set -e
[ "$code" -eq 0 ] || note "closed-pipe: exit code $code, expected 0 (141 would be SIGPIPE)"
log=$(ls "$dir"/probe-*.log 2>/dev/null | head -n 1 || true)
if [ -n "$log" ]; then
	grep -q 'frame-xrprobe finished, exit code 0' "$log" || note "closed-pipe: the log lacks the finish line"
	tail -n 3 "$log"
else
	note "closed-pipe: the probe wrote no log"
fi

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
