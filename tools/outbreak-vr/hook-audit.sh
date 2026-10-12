#!/usr/bin/env bash
# Outbreak VR (Steam Frame): checks that every OpenXR hook added to a shared ARMSX2 file is guarded.
#
# Fails if a line added since BASE (default 3cf66e03aa, where the Outbreak VR work starts) in a shared
# C/C++ file mentions the OpenXR VR layer (VR::..., OpenXR, xr..., XR_...) outside #ifdef ENABLE_OPENXR.
# Shared means everything except the copied VR layer itself: pcsx2/VR, pcsx2/Input/VRInputSource.*,
# pcsx2-qt/Settings/VRSettingsWidget.* (built only with ENABLE_OPENXR), 3rdparty/openxr and
# tools/frame-xrprobe. ARMSX2's own stereo layer is exempt: VR::StereoSettings and VR::StereoState, and
# anything under #ifdef ENABLE_VR (on wherever stereo is). Also fails if ENABLE_OPENXR no longer
# defaults to OFF.
#
# Usage: tools/outbreak-vr/hook-audit.sh [BASE]   (run from anywhere inside the repository; needs git and
# python3, and the history back to BASE)

set -euo pipefail

BASE=${1:-3cf66e03aa}
PYTHON=${PYTHON:-$(command -v python3 || command -v python || echo python3)}
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

if ! git cat-file -e "$BASE^{commit}" 2>/dev/null; then
	echo "hook-audit: base commit $BASE is not in this clone (fetch the full history)." >&2
	exit 2
fi

fail=0
if ! grep -Eq '^option\(ENABLE_OPENXR "[^"]*" OFF\)' cmake/BuildParameters.cmake; then
	echo "hook-audit: ENABLE_OPENXR must default to OFF in cmake/BuildParameters.cmake"
	fail=1
fi

files=$(git diff --name-only --diff-filter=AM "$BASE" -- \
	'*.c' '*.cc' '*.cpp' '*.h' '*.hpp' '*.inl' '*.inc' '*.mm' |
	grep -Ev '^(pcsx2/VR/|pcsx2/Input/VRInputSource\.|pcsx2-qt/Settings/VRSettingsWidget\.|3rdparty/openxr/|tools/frame-xrprobe/)' || true)

if [ -n "$files" ]; then
	# shellcheck disable=SC2086
	if ! "$PYTHON" - "$BASE" $files <<'PY'
import re
import subprocess
import sys

base = sys.argv[1]
files = sys.argv[2:]
mention = re.compile(r"\bVR::|OpenXR|openxr|\bxr[A-Z]\w*\(|\bXR_[A-Z]|\bXr[A-Z]\w*")
exempt = re.compile(r"\bVR::(StereoSettings|StereoState)\b")
cond_open = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b(.*)")
cond_else = re.compile(r"^\s*#\s*(else|elif)\b(.*)")
cond_end = re.compile(r"^\s*#\s*endif\b")
hunk = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def guard_of(kind, expr):
    expr = expr.strip()
    if kind == "ifndef":
        return None
    if re.search(r"\bENABLE_OPENXR\b", expr) and not re.search(r"!\s*defined\s*\(\s*ENABLE_OPENXR", expr):
        return "OPENXR"
    if re.search(r"\bENABLE_VR\b", expr) and not re.search(r"!\s*defined\s*\(\s*ENABLE_VR\b", expr):
        return "VR"
    return None


bad = 0
for path in files:
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except FileNotFoundError:
        continue
    # For each line: the guards it is under (a stack of the open conditionals).
    stack = []
    guards = []
    for line in lines:
        m = cond_open.match(line)
        if m:
            guards.append(tuple(stack))
            stack.append(guard_of(m.group(1), m.group(2)))
            continue
        m = cond_else.match(line)
        if m:
            guards.append(tuple(stack))
            if stack:
                # The #else of a guard is not guarded by it.
                stack[-1] = None if m.group(1) == "else" else guard_of("if", m.group(2))
            continue
        if cond_end.match(line):
            guards.append(tuple(stack))
            if stack:
                stack.pop()
            continue
        guards.append(tuple(stack))

    diff = subprocess.run(["git", "diff", "-U0", base, "--", path], capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout
    added = set()
    for d in diff.splitlines():
        m = hunk.match(d)
        if m:
            start = int(m.group(1))
            count = int(m.group(2)) if m.group(2) is not None else 1
            added.update(range(start, start + count))
    for n in sorted(added):
        if n < 1 or n > len(lines):
            continue
        text = lines[n - 1]
        code = re.sub(r"//.*", "", text)
        if not mention.search(code):
            continue
        if exempt.search(code) and not mention.search(exempt.sub("", code)):
            continue
        g = guards[n - 1]
        if "OPENXR" in g or "VR" in g:
            continue
        print(f"{path}:{n}: not under #ifdef ENABLE_OPENXR: {text.strip()}")
        bad += 1

sys.exit(1 if bad else 0)
PY
	then
		fail=1
	fi
fi

if [ "$fail" -ne 0 ]; then
	echo "hook-audit: FAILED"
	exit 1
fi
echo "hook-audit: passed ($(echo "$files" | sed '/^$/d' | wc -l) shared C/C++ files changed since $BASE)"
