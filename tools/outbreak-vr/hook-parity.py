#!/usr/bin/env python3
# Outbreak VR (Steam Frame): checks that PenguinScreen2's VR hooks are in ARMSX2, block by block.
#
# PenguinScreen2 guards its VR hooks in shared files with #ifdef ENABLE_VR; ARMSX2 guards the same hooks
# with #ifdef ENABLE_OPENXR (and has its own stereo code under #ifdef ENABLE_VR). For every ENABLE_VR block in a
# PenguinScreen2 shared file (everything but pcsx2/VR, VRInputSource and 3rdparty), this looks for the block's
# code in the guarded code (ENABLE_OPENXR or ENABLE_VR) of the same ARMSX2 file, after normalising both:
# comments dropped, string literals emptied (log wording may differ), whitespace folded, and the code split into
# statements at ; { and }. A block is "ported" when every one of its statements is in ARMSX2's guarded code of
# that file (as often as the block has it; the order is not checked, since ARMSX2's copy is a port by reading,
# not a merge). Blocks that are not, or are only partly, ported must be in the
# allowlist (hook-parity-allowlist.txt) with a reason, or the check fails. It also checks a few required terms
# in files that have no ENABLE_VR blocks (the Vulkan shader).
#
# Usage: hook-parity.py --ps2 <PenguinScreen2 checkout> [--armsx2 <ARMSX2 checkout>] [--allowlist FILE] [-v]
# Exit: 0 every block ported or allowlisted, 1 something missing, 2 bad arguments or files.

import argparse
import os
import re
import subprocess
import sys

SOURCE_EXT = (".cpp", ".h", ".hpp", ".inl", ".mm", ".cc", ".c")
EXCLUDE = re.compile(r"^(pcsx2/VR/|pcsx2/Input/VRInputSource\.|3rdparty/)")

# Terms ARMSX2 must have where PenguinScreen2 has no ENABLE_VR block (shaders are not preprocessed by the build).
REQUIRED_TERMS = [
	("bin/resources/shaders/vulkan/tfx.glsl", r"max\(vr_splits\.x,", "the first-person near floor in the linear map"),
	("bin/resources/shaders/vulkan/tfx.glsl", r"vr_d \* \(1\.0f - vr_splits\.z\)", "the head-roll x term"),
	("bin/resources/shaders/vulkan/tfx.glsl", r"vr_d \* vr_splits\.y", "the head-roll y term"),
]

COND_OPEN = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b(.*)")
COND_ELSE = re.compile(r"^\s*#\s*(else|elif)\b(.*)")
COND_END = re.compile(r"^\s*#\s*endif\b")


def is_vr_guard(kind, expr, names):
	if kind == "ifndef":
		return False
	for n in names:
		if re.search(r"\b%s\b" % n, expr) and not re.search(r"!\s*defined\s*\(\s*%s\b" % n, expr):
			return True
	return False


def strip_comments_and_strings(text):
	"""Drops comments and empties string and character literals, keeping line breaks."""
	out = []
	i, n = 0, len(text)
	while i < n:
		c = text[i]
		if c == "/" and i + 1 < n and text[i + 1] == "/":
			j = text.find("\n", i)
			i = n if j < 0 else j
			continue
		if c == "/" and i + 1 < n and text[i + 1] == "*":
			j = text.find("*/", i + 2)
			seg = text[i:n if j < 0 else j + 2]
			out.append("\n" * seg.count("\n"))
			i = n if j < 0 else j + 2
			continue
		if c in "\"'":
			# A raw string R"(...)" is rare here; treat as an ordinary literal.
			j = i + 1
			while j < n and text[j] != c:
				if text[j] == "\\":
					j += 1
				elif text[j] == "\n":
					break
				j += 1
			out.append(c + c)
			i = j + 1
			continue
		out.append(c)
		i += 1
	return "".join(out)


SEP = "\x00"


def prep(lines):
	"""Source lines to one string, each preprocessor directive a unit of its own (include paths kept)."""
	out = []
	for line in lines:
		if re.match(r"\s*#", line):
			line = SEP + re.sub(r'"([^"]*)"', r"<\1>", line.strip()) + SEP
		out.append(line)
	return "\n".join(out)


def units_of(code):
	"""Statements of a code fragment: whitespace folded, adjacent literals merged, split at ; { } and SEP."""
	flat = re.sub(r"[ \t\r\n]+", " ", code)
	flat = re.sub(r" ?([^\w\s\x00]) ?", r"\1", flat)
	flat = re.sub(r'(""|\'\')(""|\'\')+', r"\1", flat)
	units = []
	cur = []
	for ch in flat:
		if ch == SEP:
			u = "".join(cur).strip()
			if u:
				units.append(u)
			cur = []
			continue
		cur.append(ch)
		if ch in ";{}":
			u = "".join(cur).strip()
			if u:
				units.append(u)
			cur = []
	tail = "".join(cur).strip()
	if tail:
		units.append(tail)
	return [u for u in units if u not in (";",)]


def read_lines(path):
	with open(path, encoding="utf-8", errors="replace") as f:
		return f.read().replace("\r\n", "\n").split("\n")


def vr_blocks(lines, names):
	"""Top-level blocks opened by #if/#ifdef on one of names: (start line, body lines without the #else part)."""
	blocks = []
	stack = []  # (is_vr_block_start, in_else)
	cur = None
	depth_at_open = None
	for no, line in enumerate(lines, 1):
		m = COND_OPEN.match(line)
		if m:
			opens_vr = cur is None and is_vr_guard(m.group(1), m.group(2), names)
			stack.append(opens_vr)
			if opens_vr:
				cur = {"start": no, "body": [], "in_else": False}
				depth_at_open = len(stack)
				continue
			if cur is not None and not cur["in_else"]:
				cur["body"].append(line)
			continue
		m = COND_ELSE.match(line)
		if m:
			if cur is not None and len(stack) == depth_at_open:
				cur["in_else"] = True
				continue
			if cur is not None and not cur["in_else"]:
				cur["body"].append(line)
			continue
		if COND_END.match(line):
			if stack:
				stack.pop()
			if cur is not None and len(stack) < depth_at_open:
				blocks.append((cur["start"], cur["body"]))
				cur = None
				depth_at_open = None
				continue
			if cur is not None and not cur["in_else"]:
				cur["body"].append(line)
			continue
		if cur is not None and not cur["in_else"]:
			cur["body"].append(line)
	return blocks


def guarded_code(lines, names):
	"""The lines of a file under a positive guard on one of names (any depth); every other line becomes SEP, so
	no statement runs from one guarded region into the next."""
	stack = []
	keep = []
	for line in lines:
		m = COND_OPEN.match(line)
		if m:
			stack.append(is_vr_guard(m.group(1), m.group(2), names))
			keep.append(SEP)
			continue
		m = COND_ELSE.match(line)
		if m:
			if stack:
				stack[-1] = False if m.group(1) == "else" else is_vr_guard("if", m.group(2), names)
			keep.append(SEP)
			continue
		if COND_END.match(line):
			if stack:
				stack.pop()
			keep.append(SEP)
			continue
		keep.append(line if any(stack) else SEP)
	return keep


def match_ratio(needle, hay_counts, omit, includes):
	"""How many of needle's statements (less the omitted ones) are in ARMSX2's guarded code of the same file, each
	as often as the block has it (the order may differ: ARMSX2 is not a merge); an #include need only be somewhere
	in the ARMSX2 file. Returns (matched, wanted, the statements not found)."""
	want = [u for u in needle if not any(o.search(u) for o in omit)]
	left = dict(hay_counts)
	missing = []
	for u in want:
		if u.startswith("#include"):
			if u not in includes:
				missing.append(u)
			continue
		if left.get(u, 0) > 0:
			left[u] -= 1
		else:
			missing.append(u)
	return len(want) - len(missing), len(want), missing


def load_allowlist(path):
	"""Lines: <ps2 file glob> | <block anchor regex or *> | skip|omit:<regex>[;;<regex>...]|differs | <reason>"""
	entries = []
	with open(path, encoding="utf-8") as f:
		for no, raw in enumerate(f, 1):
			line = raw.strip()
			if not line or line.startswith("#"):
				continue
			parts = [p.strip() for p in line.split("|", 3)]
			if len(parts) != 4 or not parts[3]:
				sys.exit(f"{path}:{no}: want '<file> | <anchor> | <action> | <reason>'")
			file_glob, anchor, action, reason = parts
			omit = []
			if action.startswith("omit:"):
				omit = [re.compile(x) for x in action[5:].split(";;") if x]
				action = "omit"
			elif action not in ("skip", "differs"):
				sys.exit(f"{path}:{no}: action must be skip, differs or omit:<regex>")
			entries.append({"line": no, "file": file_glob, "anchor": None if anchor == "*" else re.compile(anchor),
							"action": action, "omit": omit, "reason": reason, "used": 0})
	return entries


def glob_match(glob, path):
	return re.fullmatch(re.escape(glob).replace(r"\*", ".*"), path) is not None


def ps2_files(ps2):
	try:
		out = subprocess.run(["git", "-C", ps2, "ls-files"], capture_output=True, text=True, check=True).stdout.split()
	except (OSError, subprocess.CalledProcessError):
		out = []
		for root, _, files in os.walk(ps2):
			for name in files:
				out.append(os.path.relpath(os.path.join(root, name), ps2).replace(os.sep, "/"))
	# A sparse checkout lists files it did not check out: those are left out (CI checks out every folder that has
	# ENABLE_VR blocks).
	return sorted(p for p in out if p.endswith(SOURCE_EXT) and not EXCLUDE.match(p) and os.path.exists(os.path.join(ps2, p)))


def main():
	here = os.path.dirname(os.path.abspath(__file__))
	ap = argparse.ArgumentParser()
	ap.add_argument("--ps2", required=True, help="PenguinScreen2 checkout (the commit pcsx2/VR was copied from)")
	ap.add_argument("--armsx2", default=os.path.normpath(os.path.join(here, "..", "..")))
	ap.add_argument("--allowlist", default=os.path.join(here, "hook-parity-allowlist.txt"))
	ap.add_argument("-v", "--verbose", action="store_true")
	args = ap.parse_args()
	if not os.path.isdir(os.path.join(args.ps2, "pcsx2")) or not os.path.isdir(os.path.join(args.armsx2, "pcsx2")):
		print("hook-parity: --ps2 and --armsx2 must be PenguinScreen2 and ARMSX2 checkouts", file=sys.stderr)
		return 2
	allow = load_allowlist(args.allowlist)

	rows = []
	bad = 0
	for rel in ps2_files(args.ps2):
		ps2_lines = read_lines(os.path.join(args.ps2, rel))
		blocks = vr_blocks(ps2_lines, ["ENABLE_VR"])
		if not blocks:
			continue
		ax_path = os.path.join(args.armsx2, rel)
		hay_counts = {}
		includes = set()
		if os.path.exists(ax_path):
			ax_lines = read_lines(ax_path)
			for u in units_of(strip_comments_and_strings(prep(guarded_code(ax_lines, ["ENABLE_OPENXR", "ENABLE_VR"])))):
				hay_counts[u] = hay_counts.get(u, 0) + 1
			includes = set(units_of(prep([l for l in ax_lines if re.match(r"\s*#\s*include", l)])))
		for start, body in blocks:
			raw = "\n".join(body)
			needle = units_of(strip_comments_and_strings(prep(body)))
			entry = None
			for e in allow:
				if glob_match(e["file"], rel) and (e["anchor"] is None or e["anchor"].search(raw)):
					entry = e
					break
			omit = entry["omit"] if entry and entry["action"] == "omit" else []
			got, want, missing = match_ratio(needle, hay_counts, omit, includes)
			full = want == 0 or got == want
			if entry:
				entry["used"] += 1
			if full and not omit:
				status, reason = "ported", ""
			elif full and omit:
				status, reason = "ported, less allowlisted parts", entry["reason"]
			elif entry and entry["action"] in ("skip", "differs"):
				status = "allowlisted (not ported)" if entry["action"] == "skip" else "allowlisted (differs)"
				reason = entry["reason"]
			else:
				status = "MISSING"
				reason = "not in ARMSX2: " + "  ||  ".join(missing[:3]) + ("  || ..." if len(missing) > 3 else "")
				bad += 1
			rows.append((rel, start, status, f"{got}/{want}", reason))

	for path, pattern, what in REQUIRED_TERMS:
		full = os.path.join(args.armsx2, path)
		text = "\n".join(read_lines(full)) if os.path.exists(full) else ""
		ok = re.search(pattern, text) is not None
		rows.append((path, 0, "ported" if ok else "MISSING", "term", what))
		bad += 0 if ok else 1

	width = max(len(f"{r[0]}:{r[1]}") for r in rows) if rows else 10
	for rel, start, status, ratio, reason in rows:
		if args.verbose or status != "ported":
			print(f"{(rel + ':' + str(start)).ljust(width)}  {status:<32} {ratio:>7}  {reason}")
	for e in allow:
		if e["used"] == 0:
			print(f"hook-parity: note: allowlist line {e['line']} ({e['file']} | "
				  f"{e['anchor'].pattern if e['anchor'] else '*'}) matched no block")
	counts = {}
	for r in rows:
		key = r[2].split(" (")[0]
		counts[key] = counts.get(key, 0) + 1
	summary = ", ".join(f"{v} {k}" for k, v in sorted(counts.items()))
	if bad:
		print(f"hook-parity: FAILED ({summary}). Port the block, or add it to {os.path.basename(args.allowlist)} "
			  "with the reason.")
		return 1
	print(f"hook-parity: passed ({summary}; {len(rows) - len(REQUIRED_TERMS)} PenguinScreen2 ENABLE_VR blocks and "
		  f"{len(REQUIRED_TERMS)} shader terms)")
	return 0


if __name__ == "__main__":
	sys.exit(main())
