#!/usr/bin/env python3
# Outbreak VR (Steam Frame): keeps ARMSX2's copy of PenguinScreen2's VR layer in step with PenguinScreen2.
#
# pcsx2/VR (and a few files beside it, see COPY_TREES and COPY_FILES) are copied whole from PenguinScreen2, with a
# few edits of ARMSX2's own. Those edits are kept as one small patch file per file in
# tools/outbreak-vr/vr-local-edits/<path>.patch (unified diffs against PenguinScreen2 at the commit in
# tools/outbreak-vr/PENGUINSCREEN2_COMMIT). Given a PenguinScreen2 checkout and a commit, a sync
#   1. lists what changed in the copied files since PENGUINSCREEN2_COMMIT,
#   2. takes the new versions of those files,
#   3. re-applies ARMSX2's local edits to them, and reports every hunk that no longer applies,
#   4. updates PENGUINSCREEN2_COMMIT and the source line of pcsx2/VR/VR-SOURCE.md,
#   5. runs hook-parity.py against the new commit and prints its result.
# It is a dry run unless --apply is given. PenguinScreen2 is only read, from its git objects (`git cat-file`),
# so the checkout's working tree and current branch do not matter and are never touched.
#
#   sync-vr-layer.py --ps2 <PenguinScreen2 checkout> --commit <commit or branch>          dry run
#   sync-vr-layer.py --ps2 <PenguinScreen2 checkout> --commit <commit or branch> --apply  write
#   sync-vr-layer.py --ps2 <PenguinScreen2 checkout> --record                              write the patch files
#
# --record writes the patch files from the difference between ARMSX2's copies and PenguinScreen2 at
# PENGUINSCREEN2_COMMIT. Run it after editing a copied file on purpose (and after fixing a sync that left .rej
# files). A sync refuses to run while a copied file has edits that are not recorded, because it would lose them.
# --apply re-records the patches by itself when every patch applied.
#
# Python 3.8 or newer, standard library only, plus git. Exit: 0 clean, 1 something needs a decision (a patch does
# not apply, unrecorded edits, hook parity fails), 2 bad arguments or an unusable checkout.

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap

COMMIT_FILE = "tools/outbreak-vr/PENGUINSCREEN2_COMMIT"
PATCH_DIR = "tools/outbreak-vr/vr-local-edits"
SOURCE_DOC = "pcsx2/VR/VR-SOURCE.md"
PARITY_TOOL = "tools/outbreak-vr/hook-parity.py"

# Where each copied file lives in both repos (the paths are the same in PenguinScreen2 and ARMSX2). A folder is
# copied with everything in it, so a file PenguinScreen2 adds there is picked up (and reported as new).
COPY_TREES = ("pcsx2/VR", "bin/resources/vr-profiles")
COPY_FILES = (
    "pcsx2/Input/VRInputSource.cpp",
    "pcsx2/Input/VRInputSource.h",
    "pcsx2-qt/Settings/VRSettingsWidget.cpp",
    "pcsx2-qt/Settings/VRSettingsWidget.h",
    "pcsx2-qt/Settings/VRSettingsWidget.ui",
    "tools/outbreak-vr/pine.py",
)
# Not copied, on purpose: pcsx2/VR/StereoSettings.* (ARMSX2's own, not in PenguinScreen2) and VR-SOURCE.md.
# A sync only ever touches files PenguinScreen2 has.

SHARED_NOTE = ("every ARMSX2 stereo build (Android, iOS, macOS, Windows) compiles this file, not only the Steam "
               "Frame build: check that whatever is new in it is under #ifdef ENABLE_OPENXR")
FILE_NOTES = {
    "pcsx2/VR/StereoState.h": SHARED_NOTE,
    "pcsx2/VR/StereoState.cpp": SHARED_NOTE,
}
SOURCE_EXT = (".cpp", ".h", ".hpp", ".inl", ".ui")

# Files PenguinScreen2 has that ARMSX2 does not copy but ports by hand (Config.h's VROptions, the online locks in
# VMManager, the hooks in the GS, input and Qt code, the Vulkan shader, CMake lists). They are found, not listed:
# any file that changed between the two commits and whose text mentions the VR layer.
HAND_PORTED_EXT = (".cpp", ".h", ".hpp", ".inl", ".mm", ".cc", ".c", ".glsl", ".txt", ".cmake")
MENTIONS_VR = re.compile(rb"ENABLE_VR|VR::|VROptions|vr_splits|IsOnlineLocked|VRManager|OpenXR")

HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


class ToolError(Exception):
    pass


def say(text=""):
    print(text)


# ---------------------------------------------------------------- git (PenguinScreen2 is read through here)

class Git:
    def __init__(self, repo):
        self.repo = repo
        self._cat = None

    def run(self, *args, check=True):
        try:
            p = subprocess.run(["git", "-C", self.repo] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except OSError as e:
            raise ToolError(f"cannot run git: {e}")
        if check and p.returncode != 0:
            raise ToolError("git %s failed: %s" % (" ".join(args[:2]), p.stderr.decode("utf-8", "replace").strip()))
        return p

    def out(self, *args, check=True):
        return self.run(*args, check=check).stdout.decode("utf-8", "replace")

    def resolve(self, rev):
        if rev.startswith("-"):
            return None
        p = self.run("rev-parse", "--verify", "--quiet", rev + "^{commit}", check=False)
        return p.stdout.decode().strip() if p.returncode == 0 else None

    def info(self, sha):
        date, subject = self.out("log", "-1", "--format=%cs%n%s", sha).split("\n", 1)
        return date.strip(), subject.strip()

    def tree_files(self, sha):
        """Paths of the copied files in a PenguinScreen2 commit."""
        p = self.run("ls-tree", "-r", "--name-only", "-z", sha, "--", *(COPY_TREES + COPY_FILES))
        return sorted(n.decode("utf-8", "surrogateescape") for n in p.stdout.split(b"\0") if n)

    def read(self, sha, path):
        """The file's bytes at that commit, or None."""
        if self._cat is None:
            self._cat = subprocess.Popen(["git", "-C", self.repo, "cat-file", "--batch"], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self._cat.stdin.write(("%s:%s\n" % (sha, path)).encode("utf-8", "surrogateescape"))
        self._cat.stdin.flush()
        head = self._cat.stdout.readline().split()
        if len(head) < 3 or head[1] != b"blob":
            return None
        size = int(head[2])
        data = self._cat.stdout.read(size)
        self._cat.stdout.read(1)
        return data

    def close(self):
        if self._cat is not None:
            self._cat.stdin.close()
            self._cat.wait()
            self._cat = None


# ---------------------------------------------------------------- text and patches

def is_text(data):
    return b"\0" not in data


def to_lines(data):
    """Bytes to (lines, crlf). Lines keep their "\n" (the last one may not have it); CRLF is folded to LF."""
    text = data.decode("utf-8", "surrogateescape")
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    parts = text.split("\n")
    lines = [p + "\n" for p in parts[:-1]]
    if parts[-1] != "":
        lines.append(parts[-1])
    return lines, crlf


def from_lines(lines, crlf):
    text = "".join(lines)
    if crlf:
        text = text.replace("\n", "\r\n")
    return text.encode("utf-8", "surrogateescape")


def count_changes(a, b):
    plus = minus = 0
    for tag, a1, a2, b1, b2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if tag in ("replace", "delete"):
            minus += a2 - a1
        if tag in ("replace", "insert"):
            plus += b2 - b1
    return plus, minus


class Hunk:
    def __init__(self, old_start, old_len, new_start, new_len):
        self.old_start, self.old_len, self.new_start, self.new_len = old_start, old_len, new_start, new_len
        self.lines = []  # (tag, text) with tag one of " ", "-", "+"; text keeps its "\n"

    def block(self, drop):
        return [t for tag, t in self.lines if tag != drop]

    def context_at(self, start):
        n = 0
        seq = self.lines if start else reversed(self.lines)
        for tag, _ in seq:
            if tag != " ":
                break
            n += 1
        return n

    def format(self):
        def rng(start, length):
            return str(start) if length == 1 else "%d,%d" % (start, length)
        out = ["@@ -%s +%s @@\n" % (rng(self.old_start, self.old_len), rng(self.new_start, self.new_len))]
        for tag, text in self.lines:
            if text.endswith("\n"):
                out.append(tag + text)
            else:
                out.append(tag + text + "\n\\ No newline at end of file\n")
        return "".join(out)


def make_hunks(a, b, context=3):
    hunks = []
    for group in difflib.SequenceMatcher(None, a, b, autojunk=False).get_grouped_opcodes(context):
        i1, i2, j1, j2 = group[0][1], group[-1][2], group[0][3], group[-1][4]
        h = Hunk(i1 + 1 if i2 > i1 else i1, i2 - i1, j1 + 1 if j2 > j1 else j1, j2 - j1)
        for tag, a1, a2, b1, b2 in group:
            if tag == "equal":
                h.lines += [(" ", t) for t in a[a1:a2]]
                continue
            if tag in ("replace", "delete"):
                h.lines += [("-", t) for t in a[a1:a2]]
            if tag in ("replace", "insert"):
                h.lines += [("+", t) for t in b[b1:b2]]
        hunks.append(h)
    return hunks


def format_patch(path, hunks):
    head = ("# ARMSX2 (Steam Frame) local edit to %s: a diff against PenguinScreen2 at the commit in\n"
            "# tools/outbreak-vr/PENGUINSCREEN2_COMMIT, with line endings folded to LF. Written by\n"
            "# tools/outbreak-vr/sync-vr-layer.py --record; do not edit by hand.\n"
            "--- a/%s\n+++ b/%s\n" % (path, path, path))
    return head + "".join(h.format() for h in hunks)


def parse_patch(text, name):
    lines = text.replace("\r\n", "\n").split("\n")
    i = 0
    while i < len(lines) and not lines[i].startswith("--- "):
        i += 1
    if i + 1 >= len(lines) or not lines[i + 1].startswith("+++ "):
        raise ToolError("%s: no '--- ' / '+++ ' header" % name)
    i += 2
    hunks = []

    def no_eol(h):
        tag, text = h.lines[-1]
        h.lines[-1] = (tag, text[:-1] if text.endswith("\n") else text)

    while i < len(lines):
        m = HUNK_RE.match(lines[i])
        if not m:
            if lines[i].strip() == "":
                i += 1
                continue
            raise ToolError("%s, line %d: expected a hunk header" % (name, i + 1))
        h = Hunk(int(m.group(1)), 1 if m.group(2) is None else int(m.group(2)),
                 int(m.group(3)), 1 if m.group(4) is None else int(m.group(4)))
        i += 1
        old_seen = new_seen = 0
        while old_seen < h.old_len or new_seen < h.new_len:
            if i >= len(lines):
                raise ToolError("%s: the last hunk is cut short" % name)
            raw = lines[i]
            i += 1
            if raw.startswith("\\"):
                no_eol(h)
                continue
            tag = raw[:1] or " "
            if tag not in " +-":
                raise ToolError("%s, line %d: not a hunk line" % (name, i))
            h.lines.append((tag, raw[1:] + "\n"))
            if tag != "+":
                old_seen += 1
            if tag != "-":
                new_seen += 1
        while i < len(lines) and lines[i].startswith("\\"):
            no_eol(h)
            i += 1
        hunks.append(h)
    return hunks


class Outcome:
    def __init__(self, hunk, status, at=0, offset=0, fuzz=0, note=""):
        self.hunk, self.status, self.at, self.offset, self.fuzz, self.note = hunk, status, at, offset, fuzz, note


def find_block(target, index, block, expect, floor):
    """Index in target of the exact block, at or after floor, nearest to expect (the earlier one on a tie)."""
    if not block:
        return min(max(expect, floor), len(target))
    best = None
    for i in index.get(block[0], ()):
        if i < floor or target[i:i + len(block)] != block:
            continue
        d = abs(i - expect)
        if best is None or d < best[0]:
            best = (d, i)
    return None if best is None else best[1]


def trunc(text, width):
    return text if len(text) <= width else text[:width - 3] + "..."


def shorten(line):
    s = repr(line.strip())
    return s if len(s) <= 90 else s[:87] + "...'"


def closest(block, target, index, expect):
    votes = {}
    for k, line in enumerate(block):
        if line.strip() in ("", "{", "}"):
            continue
        for i in index.get(line, ())[:5000]:
            votes[i - k] = votes.get(i - k, 0) + 1
    if not votes:
        return "none of its lines is in the new copy"
    start = max(votes, key=lambda s: (votes[s], -abs(s - expect)))
    msg = "closest place: line %d of the new copy, where %d of its %d lines match" % (max(start, 0) + 1, votes[start], len(block))
    for k, line in enumerate(block):
        j = start + k
        got = target[j] if 0 <= j < len(target) else None
        if got != line:
            msg += "; first difference at its line %d: it wants %s, the new copy has %s" % (
                k + 1, shorten(line), "nothing (end of file)" if got is None else shorten(got))
            break
    return msg


def apply_hunks(target, hunks, fuzz=0):
    """Applies hunks to target (a list of lines). Each hunk is looked for as it was recorded, at the nearest place
    to where the last one landed (so the line numbers in the patch are only a hint). A hunk whose result is
    already there is skipped ("already"). Returns (the new lines, one Outcome per hunk)."""
    index = {}
    for i, line in enumerate(target):
        index.setdefault(line, []).append(i)
    result, outcomes = [], []
    pos = drift = 0
    for h in hunks:
        nominal = h.old_start - 1 if h.old_len else h.old_start
        placed = None
        for f in range(fuzz + 1):
            lead, trail = min(f, h.context_at(True)), min(f, h.context_at(False))
            sub = h.lines[lead:len(h.lines) - trail]
            old_b = [t for tag, t in sub if tag != "+"]
            i = find_block(target, index, old_b, nominal + lead + drift, pos)
            if i is not None:
                placed = (i, old_b, [t for tag, t in sub if tag != "-"], lead, f)
                break
        if placed:
            i, old_b, new_b, lead, f = placed
            result.extend(target[pos:i])
            outcomes.append(Outcome(h, "applied", at=len(result) + 1 - lead, offset=i - (nominal + lead), fuzz=f))
            drift = i - (nominal + lead)
            result.extend(new_b)
            pos = i + len(old_b)
            continue
        new_b = h.block("-")
        j = find_block(target, index, new_b, nominal + drift, pos) if new_b else None
        if j is not None:
            result.extend(target[pos:j + len(new_b)])
            pos = j + len(new_b)
            outcomes.append(Outcome(h, "already", at=len(result) - len(new_b) + 1, offset=j - nominal))
            drift = j - nominal
            continue
        outcomes.append(Outcome(h, "failed", note=closest(h.block("+"), target, index, nominal + drift)))
    result.extend(target[pos:])
    return result, outcomes


# ---------------------------------------------------------------- the ARMSX2 working tree

def working_path(root, rel):
    return os.path.join(root, *rel.split("/"))


def read_working(root, rel):
    try:
        with open(working_path(root, rel), "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def write_working(root, rel, data):
    path = working_path(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def load_patches(root):
    """{copied file path: patch text} from tools/outbreak-vr/vr-local-edits."""
    found = {}
    base = working_path(root, PATCH_DIR)
    for folder, _, files in os.walk(base):
        for name in files:
            if name.endswith(".patch"):
                full = os.path.join(folder, name)
                rel = os.path.relpath(full, base).replace(os.sep, "/")[:-len(".patch")]
                with open(full, "rb") as f:
                    found[rel] = f.read().decode("utf-8", "surrogateescape")
    return found


def find_rejects(root, paths):
    return [p + ".rej" for p in paths if os.path.exists(working_path(root, p + ".rej"))]


def read_recorded_commit(root):
    data = read_working(root, COMMIT_FILE)
    sha = data.decode("ascii", "replace").strip() if data else ""
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ToolError("%s does not hold a full commit id" % COMMIT_FILE)
    return sha


# ---------------------------------------------------------------- the plan: what a sync would do

class Entry:
    def __init__(self, path):
        self.path = path
        self.upstream = "unchanged"   # unchanged, modified, added, removed (in PenguinScreen2, old commit to new)
        self.plus = self.minus = 0
        self.binary = False
        self.patch = None             # parsed hunks of its patch file, if it has one
        self.outcomes = []
        self.blockers = []            # a sync must not run: its edits would be lost
        self.conflicts = []           # a hunk or a file that cannot be placed
        self.notes = []
        self.write = None             # bytes a sync writes, None when ARMSX2's file stays as it is
        self.reject = None            # text for <path>.rej
        self.missing = False          # ARMSX2 has no such file


def make_plan(git, root, old_sha, new_sha, fuzz):
    old_set, new_set = set(git.tree_files(old_sha)), set(git.tree_files(new_sha))
    patches = load_patches(root)
    entries = []
    for path in sorted(old_set | new_set):
        e = Entry(path)
        entries.append(e)
        old = git.read(old_sha, path) if path in old_set else None
        new = git.read(new_sha, path) if path in new_set else None
        cur = read_working(root, path)
        patch_text = patches.pop(path, None)
        e.missing = cur is None
        if new is None:
            e.upstream = "removed"
            e.notes.append("removed in PenguinScreen2; ARMSX2's copy stays (delete it, and its CMake entry, by hand)")
            if patch_text is not None:
                e.notes.append("its patch file in %s is stale now: delete it with the copy" % PATCH_DIR)
            continue
        if old is None:
            e.upstream = "added"
            if path.endswith(SOURCE_EXT):
                e.notes.append("new file: it is in no CMakeLists.txt yet; add it by hand if it is source")
            if cur is None:
                e.write = from_lines(to_lines(new)[0], False) if is_text(new) else new
            elif cur.replace(b"\r\n", b"\n") != new.replace(b"\r\n", b"\n"):
                e.conflicts.append("ARMSX2 already has a different file by this name; not overwritten")
            continue
        text_ok = is_text(old) and is_text(new) and (cur is None or is_text(cur))
        if patch_text is not None:
            try:
                e.patch = parse_patch(patch_text, path + ".patch")
            except ToolError as err:
                e.blockers.append("its patch file cannot be read: %s" % err)
        if not text_ok:
            e.binary = True
            if old != new:
                e.upstream = "modified"
            if e.patch is not None:
                e.blockers.append("a patch cannot apply to a binary file")
            if cur is not None and cur != old:
                e.blockers.append("a binary file that differs from PenguinScreen2's old copy: copy it by hand")
            if e.upstream == "modified" or cur is None:
                e.write = new
            continue
        old_lines, _ = to_lines(old)
        new_lines, _ = to_lines(new)
        cur_lines, cur_crlf = to_lines(cur) if cur is not None else (None, False)
        if old_lines != new_lines:
            e.upstream = "modified"
            e.plus, e.minus = count_changes(old_lines, new_lines)
        if e.patch is not None:
            expected, outs = apply_hunks(old_lines, e.patch, 0)
            if any(o.status != "applied" for o in outs):
                e.blockers.append("its patch does not apply to the recorded commit (run --record)")
                expected = None
        else:
            expected = old_lines
        if cur_lines is not None and expected is not None and cur_lines != expected:
            plus, minus = count_changes(expected, cur_lines)
            e.blockers.append("ARMSX2's copy has edits that are not recorded in %s (+%d -%d lines; run --record)"
                              % (PATCH_DIR, plus, minus))
        if e.patch is not None:
            result, e.outcomes = apply_hunks(new_lines, e.patch, fuzz)
        else:
            result = new_lines
        failed = [o for o in e.outcomes if o.status == "failed"]
        if failed:
            e.conflicts.append("%d of %d hunks do not apply" % (len(failed), len(e.outcomes)))
            e.reject = ("# These hunks of ARMSX2's local edit to %s did not apply to PenguinScreen2 %s.\n"
                        "# Make the edit by hand in %s, delete this file, then run sync-vr-layer.py --record.\n"
                        % (path, new_sha, path)
                        + "--- a/%s\n+++ b/%s\n" % (path, path) + "".join(o.hunk.format() for o in failed))
        if cur_lines is None:
            e.missing = True
            e.write = from_lines(result, False)
        elif result != cur_lines:
            e.write = from_lines(result, cur_crlf)
    return entries, sorted(patches)


def hand_ported_changes(git, old_sha, new_sha, copied):
    """[(path, plus, minus)]: files outside the copied set that changed between the commits and mention the VR
    layer. ARMSX2 ports those by hand, so a change in one of them is work no patch carries over."""
    out = git.run("diff", "--numstat", "--no-renames", "-z", old_sha, new_sha).stdout
    found = []
    for rec in out.split(b"\0"):
        if not rec:
            continue
        plus, minus, path = rec.decode("utf-8", "surrogateescape").split("\t", 2)
        if path in copied or path.startswith(("tools/", "3rdparty/")) or not path.endswith(HAND_PORTED_EXT):
            continue
        data = git.read(new_sha, path) or git.read(old_sha, path)
        if data and MENTIONS_VR.search(data):
            found.append((path, plus, minus))
    return sorted(found)


# ---------------------------------------------------------------- --record

def record_patches(git, root, sha, write):
    """Writes (or only lists) one patch per copied file whose ARMSX2 copy differs from PenguinScreen2 at sha.
    Returns [(path, action, detail)]; the actions are written, unchanged, removed, skipped."""
    actions = []
    files = git.tree_files(sha)
    existing = load_patches(root)
    for path in files:
        blob = git.read(sha, path)
        cur = read_working(root, path)
        patch_path = PATCH_DIR + "/" + path + ".patch"
        if cur is None:
            actions.append((path, "skipped", "ARMSX2 has no such file"))
            continue
        if not (is_text(blob) and is_text(cur)):
            if blob != cur:
                actions.append((path, "skipped", "a binary file that differs; a patch cannot hold it"))
            continue
        a, _ = to_lines(blob)
        b, _ = to_lines(cur)
        hunks = make_hunks(a, b)
        old_text = existing.pop(path, None)
        if not hunks:
            if old_text is not None:
                if write:
                    os.remove(working_path(root, patch_path))
                actions.append((path, "removed", "no local edits any more"))
            continue
        text = format_patch(path, hunks)
        detail = "%d hunk%s, +%d -%d" % (len(hunks), "" if len(hunks) == 1 else "s", sum(1 for h in hunks for t, _ in h.lines if t == "+"),
                                        sum(1 for h in hunks for t, _ in h.lines if t == "-"))
        if old_text is not None and old_text.replace("\r\n", "\n") == text:
            actions.append((path, "unchanged", detail))
            continue
        if write:
            write_working(root, patch_path, text.encode("utf-8", "surrogateescape"))
        actions.append((path, "written", detail))
    for path in sorted(existing):
        actions.append((path, "skipped", "a patch for a file that is not a copied file (delete it by hand)"))
    return actions


# ---------------------------------------------------------------- VR-SOURCE.md

SOURCE_LINE_RE = re.compile(r"branch `([^`]*)`,\s+commit\s+\*\*([0-9a-f]{40})\*\*\s+\(.*?\)\.\s+It\s+is\s+built", re.S)


def pick_branch(git, new_sha, commit_arg, branch_arg, old_branch):
    """The branch name for VR-SOURCE.md: --branch, the branch the commit argument names, the old one if the
    commit is on it, the only branch that has the commit. Returns (name, warning)."""
    if branch_arg:
        return branch_arg, ""
    for ref in ("refs/heads/" + commit_arg, "refs/remotes/origin/" + commit_arg):
        if git.resolve(ref) == new_sha:
            return commit_arg, ""
    if old_branch:
        old_tip = git.resolve("refs/heads/" + old_branch)
        if old_tip and git.run("merge-base", "--is-ancestor", new_sha, old_tip, check=False).returncode == 0:
            return old_branch, ""
    having = [b for b in git.out("for-each-ref", "--contains", new_sha, "--format=%(refname:short)",
                                 "refs/heads", check=False).split() if b]
    if len(having) == 1:
        return having[0], ""
    return old_branch or "?", ("the branch name in VR-SOURCE.md is not changed: %d branches have the commit (%s); "
                               "pass --branch" % (len(having), ", ".join(having[:6])))


def source_line(branch, sha, date, subject):
    subject = subject.replace('"', "'")
    if len(subject) > 64:
        subject = subject[:64].rsplit(" ", 1)[0] + " ..."
    return "branch `%s`, commit **%s** (%s, \"%s\"). It is built" % (branch, sha, date, subject)


def rewrite_source_doc(text, line):
    """The doc's source line replaced, and its paragraph wrapped again; None if the line is not found."""
    m = SOURCE_LINE_RE.search(text)
    if not m:
        return None
    text = text[:m.start()] + line + text[m.end():]
    m = re.search(r"branch `[^`]*`, commit \*\*", text)
    start = text.rfind("\n\n", 0, m.start()) + 2
    end = text.find("\n\n", m.end())
    end = len(text) if end < 0 else end
    paragraph = textwrap.fill(" ".join(text[start:end].split()), width=118, break_long_words=False,
                              break_on_hyphens=False)
    return text[:start] + paragraph + text[end:]


def stale_mentions(root, old_sha):
    """Lines of the docs that still name the old commit."""
    found = []
    for rel in (SOURCE_DOC, "OUTBREAK-VR.md"):
        data = read_working(root, rel)
        if data is None:
            continue
        for no, line in enumerate(data.decode("utf-8", "replace").split("\n"), 1):
            if old_sha[:7] in line and old_sha not in line:
                found.append("%s:%d" % (rel, no))
    return found


# ---------------------------------------------------------------- hook-parity

def run_parity(git, root, new_sha, verbose):
    """hook-parity.py reads a checkout; this writes the files of PenguinScreen2 at new_sha that it needs (every
    source file with ENABLE_VR in it) to a temporary folder, and runs it against ARMSX2 as it is."""
    tool = working_path(root, PARITY_TOOL)
    if not os.path.exists(tool):
        return 1, ["%s is missing" % PARITY_TOOL]
    tmp = tempfile.mkdtemp(prefix="vr-sync-ps2-")
    try:
        os.makedirs(os.path.join(tmp, "pcsx2"))
        out = git.run("grep", "-l", "-z", "-F", "-e", "ENABLE_VR", new_sha, "--", "pcsx2", "pcsx2-qt",
                      "pcsx2-gsrunner", "common", check=False).stdout
        for item in out.split(b"\0"):
            path = item.decode("utf-8", "surrogateescape")[len(new_sha) + 1:]
            if not path.endswith((".cpp", ".h", ".hpp", ".inl", ".mm", ".cc", ".c")):
                continue
            data = git.read(new_sha, path)
            if data is None:
                continue
            dest = os.path.join(tmp, *path.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(data)
        cmd = [sys.executable, tool, "--ps2", tmp, "--armsx2", root] + (["-v"] if verbose else [])
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.returncode, p.stdout.decode("utf-8", "replace").rstrip().split("\n")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------- the report

def short(sha):
    return sha[:12]


def describe_outcomes(e, verbose):
    lines = []
    for n, o in enumerate(e.outcomes, 1):
        h = o.hunk
        was = "was line %d" % (h.old_start if h.old_len else h.old_start + 1)
        if o.status == "failed":
            lines.append("hunk %d (%s) DOES NOT APPLY: %s" % (n, was, o.note))
        elif o.status == "already":
            lines.append("hunk %d (%s) is already in the new PenguinScreen2 copy (ported back): it is skipped, "
                         "and --record will drop it" % (n, was))
        elif verbose or o.offset or o.fuzz:
            extra = ""
            if o.fuzz:
                extra = ", with fuzz %d: check it" % o.fuzz
            lines.append("hunk %d applies at line %d (%s, moved by %+d%s)" % (n, o.at, was, o.offset, extra))
    return lines


def report_changes(git, entries, hand, old_sha, new_sha, ps2):
    changed = [e for e in entries if e.upstream != "unchanged"]
    say("1. What changed in PenguinScreen2 since %s" % short(old_sha))
    if not changed:
        say("   Copied files: nothing, every one is the same in both commits.")
        report_hand_ported(hand, old_sha, new_sha, ps2)
        return
    plus = sum(e.plus for e in changed)
    minus = sum(e.minus for e in changed)
    say("   Copied files: %d of %d changed (+%d -%d lines):" % (len(changed), len(entries), plus, minus))
    for e in changed:
        counts = "binary" if e.binary else ("+%d -%d" % (e.plus, e.minus) if e.upstream == "modified" else "")
        say("     %-9s %-50s %s" % (e.upstream, e.path, counts))
        for note in e.notes:
            say("               note: %s" % note)
        if e.upstream == "modified" and e.path in FILE_NOTES:
            say("               note: %s" % FILE_NOTES[e.path])
    paths = COPY_TREES + COPY_FILES
    total = git.out("rev-list", "--count", "%s..%s" % (old_sha, new_sha), check=False).strip()
    log = git.out("log", "--format=%h %s", "%s..%s" % (old_sha, new_sha), "--", *paths, check=False).strip().split("\n")
    log = [l for l in log if l]
    say("   %d of the %s commits in the range touch them%s" % (len(log), total or "?", ":" if log else "."))
    for l in log[:25]:
        say("     %s" % (l if len(l) <= 110 else l[:107] + "..."))
    if len(log) > 25:
        say("     ... and %d more" % (len(log) - 25))
    report_hand_ported(hand, old_sha, new_sha, ps2)


def report_hand_ported(hand, old_sha, new_sha, ps2):
    if not hand:
        say("   Files ARMSX2 ports by hand: none of them changed.")
        return
    say("   Files ARMSX2 ports by hand that changed (not copied; they mention the VR layer). Read each diff, then port it or")
    say("   leave it: git -C %s diff %s %s -- <path>" % (ps2, short(old_sha), short(new_sha)))
    for path, plus, minus in hand:
        say("     %-9s %-50s +%s -%s" % ("changed", path, plus, minus))


def report_copy(entries):
    writes = [e for e in entries if e.write is not None]
    say("2. New versions of the copied files")
    if not writes:
        say("   Nothing to write: ARMSX2's copies are what the sync would produce.")
        return
    say("   %d file%s to write (a file with local edits is written with its edits applied):"
        % (len(writes), "" if len(writes) == 1 else "s"))
    for e in writes:
        kind = "create" if e.missing else "update"
        say("     %-7s %s" % (kind, e.path))


def report_patches(entries, orphans, verbose):
    patched = [e for e in entries if e.patch is not None]
    say("3. ARMSX2's local edits (%s)" % PATCH_DIR)
    if not patched:
        say("   No patch files.")
    clean = 0
    for e in patched:
        failed = [o for o in e.outcomes if o.status == "failed"]
        already = [o for o in e.outcomes if o.status == "already"]
        if e.blockers and not e.outcomes:
            state = "BLOCKED"
        elif failed:
            state = "%d of %d hunks DO NOT APPLY" % (len(failed), len(e.outcomes))
        elif already:
            state = "all apply (%d already in PenguinScreen2)" % len(already)
            clean += 1
        else:
            state = "all %d hunk%s" % (len(e.outcomes), " applies" if len(e.outcomes) == 1 else "s apply")
            clean += 1
        say("   %-52s %s%s" % (e.path, state, "; PenguinScreen2 changed this file too" if e.upstream == "modified" else ""))
        for line in describe_outcomes(e, verbose):
            say("       %s" % line)
        if e.reject:
            say("       the failed hunks go to %s.rej" % e.path)
    for e in entries:
        for b in e.blockers:
            say("   BLOCKER  %s: %s" % (e.path, b))
        for c in e.conflicts:
            if not c.endswith("do not apply"):
                say("   CONFLICT %s: %s" % (e.path, c))
    for path in orphans:
        say("   note: %s.patch is for a file that is not a copied file in either commit; delete it by hand" % path)
    if patched:
        say("   %d of %d patches apply cleanly." % (clean, len(patched)))


def old_source_branch(root):
    data = read_working(root, SOURCE_DOC)
    m = SOURCE_LINE_RE.search(data.decode("utf-8", "replace")) if data else None
    return m.group(1) if m else ""


def run(args):
    root = os.path.abspath(args.armsx2)
    if not os.path.isdir(os.path.join(root, "pcsx2", "VR")):
        raise ToolError("--armsx2 %s is not an ARMSX2 checkout" % root)
    if not os.path.isdir(args.ps2):
        raise ToolError("--ps2 %s is not a folder" % args.ps2)
    git = Git(args.ps2)
    try:
        git.out("rev-parse", "--git-dir")
        old_sha = read_recorded_commit(root)
        if git.resolve(old_sha) is None:
            raise ToolError("the recorded commit %s is not in %s (fetch it)" % (short(old_sha), args.ps2))
        if args.record:
            return run_record(git, root, old_sha, args)
        if not args.commit:
            raise ToolError("--commit is required (a commit or branch of the PenguinScreen2 checkout)")
        new_sha = git.resolve(args.commit)
        if new_sha is None:
            raise ToolError("%s is not a commit in %s" % (args.commit, args.ps2))
        return run_sync(git, root, old_sha, new_sha, args)
    finally:
        git.close()


def run_record(git, root, sha, args):
    write = not args.dry_run
    say("sync-vr-layer --record%s" % ("" if write else " --dry-run (nothing is written)"))
    say("  patches against PenguinScreen2 %s (PENGUINSCREEN2_COMMIT), in %s" % (short(sha), PATCH_DIR))
    rejects = find_rejects(root, git.tree_files(sha))
    if rejects:
        for r in rejects:
            say("  %s is still there" % r)
        raise ToolError("resolve the .rej files above (make the edit by hand, delete the .rej) before recording")
    actions = record_patches(git, root, sha, write)
    counts = {}
    for path, action, detail in actions:
        counts[action] = counts.get(action, 0) + 1
        if action != "unchanged" or args.verbose:
            say("  %-9s %-50s %s" % (action, path, detail))
    say("  %s" % (", ".join("%d %s" % (v, k) for k, v in sorted(counts.items())) or "no local edits, no patches"))
    return 1 if any(a[1] == "skipped" for a in actions) else 0


def run_sync(git, root, old_sha, new_sha, args):
    apply = args.apply
    date_old, subj_old = git.info(old_sha)
    date_new, subj_new = git.info(new_sha)
    say("sync-vr-layer: %s" % ("APPLY" if apply else "DRY RUN (nothing is written; --apply writes)"))
    say("  PenguinScreen2  %s" % args.ps2)
    say("  from            %s  %s  %s (PENGUINSCREEN2_COMMIT)" % (short(old_sha), date_old, trunc(subj_old, 70)))
    say("  to              %s  %s  %s (%s)" % (short(new_sha), date_new, trunc(subj_new, 70), args.commit))
    if old_sha != new_sha and git.run("merge-base", "--is-ancestor", old_sha, new_sha, check=False).returncode != 0:
        say("  note: %s is not an ancestor of %s (a sideways or backwards sync)" % (short(old_sha), short(new_sha)))
    say()

    entries, orphans = make_plan(git, root, old_sha, new_sha, args.fuzz)
    rejects = find_rejects(root, [e.path for e in entries])
    hand = hand_ported_changes(git, old_sha, new_sha, set(e.path for e in entries))
    report_changes(git, entries, hand, old_sha, new_sha, args.ps2)
    say()
    report_copy(entries)
    say()
    report_patches(entries, orphans, args.verbose)
    say()

    # 4. the commit and the doc
    branch, warn = pick_branch(git, new_sha, args.commit, args.branch, old_source_branch(root))
    say("4. %s and %s" % (COMMIT_FILE.rsplit("/", 1)[1], SOURCE_DOC.rsplit("/", 1)[1]))
    if old_sha == new_sha:
        say("   No change: the commit is the recorded one.")
    else:
        say("   %s %s -> %s" % ("Setting" if apply else "Would set", short(old_sha), short(new_sha)))
        say("   Source line: %s" % source_line(branch, new_sha, date_new, subj_new)[:-len(" It is built")].rstrip())
        if warn:
            say("   warning: %s" % warn)
        mentions = stale_mentions(root, old_sha)
        if mentions:
            say("   Still naming %s after the sync (update by hand; the hook-site line numbers in the table are "
                "at that commit): %s" % (short(old_sha)[:7], ", ".join(mentions[:12])))
    say()

    problems = []
    blockers = [e for e in entries if e.blockers]
    conflicts = [e for e in entries if e.conflicts]
    for e in blockers:
        problems.append("%s: %s" % (e.path, "; ".join(e.blockers)))
    for e in conflicts:
        problems.append("%s: %s" % (e.path, "; ".join(e.conflicts)))
    if rejects:
        problems.append("old reject files are still there: %s" % ", ".join(rejects))
    written = False
    if apply:
        if blockers or rejects:
            say("   NOT APPLIED: %s." % ("a copied file has edits that would be lost (see BLOCKER above)" if blockers
                                         else "resolve and delete the old .rej files first"))
        else:
            written = True
            for e in entries:
                if e.write is not None:
                    write_working(root, e.path, e.write)
                if e.reject:
                    write_working(root, e.path + ".rej", e.reject.encode("utf-8", "surrogateescape"))
            if old_sha != new_sha:
                write_working(root, COMMIT_FILE, (new_sha + "\n").encode("ascii"))
                doc = read_working(root, SOURCE_DOC)
                if doc is not None:
                    text = doc.decode("utf-8")
                    crlf = "\r\n" in text
                    new_text = rewrite_source_doc(text.replace("\r\n", "\n"), source_line(branch, new_sha, date_new, subj_new))
                    if new_text is None:
                        problems.append("the source line in %s was not found; update it by hand" % SOURCE_DOC)
                    else:
                        write_working(root, SOURCE_DOC, (new_text.replace("\n", "\r\n") if crlf else new_text).encode("utf-8"))
            say("   Written: %d file%s, %s." % (sum(1 for e in entries if e.write is not None),
                                                 "" if sum(1 for e in entries if e.write is not None) == 1 else "s",
                                                 "the commit file and the source line" if old_sha != new_sha else "no commit change"))
            if conflicts:
                say("   %d file%s left with .rej files. Make those edits by hand, delete the .rej files, then run "
                    "--record" % (len(conflicts), "" if len(conflicts) == 1 else "s"))
                say("   (the patch files are not re-recorded while a hunk is open).")
            else:
                actions = record_patches(git, root, new_sha, True)
                changed = [a for a in actions if a[1] in ("written", "removed")]
                say("   Patch files re-recorded against %s: %d written or removed." % (short(new_sha), len(changed)))
                again, _ = make_plan(git, root, new_sha, new_sha, 0)
                dirty = [e.path for e in again if e.write is not None or e.blockers or e.conflicts]
                if dirty:
                    problems.append("self-check: a dry run against the new commit still wants to change: %s"
                                    % ", ".join(dirty))
                else:
                    say("   Self-check: a dry run against the new commit shows no changes.")
        say()

    # 5. hook parity
    say("5. hook-parity.py against PenguinScreen2 %s" % short(new_sha))
    if args.no_parity:
        say("   Skipped (--no-parity).")
    else:
        code, lines = run_parity(git, root, new_sha, args.verbose)
        for l in lines:
            if args.verbose or code != 0 or l.startswith("hook-parity:"):
                say("   %s" % trunc(l, 230))
        if code == 0 and not args.verbose:
            say("   (-v lists every block and why the allowlisted ones are)")
        if code != 0:
            problems.append("hook-parity.py exited %d" % code)
    say()

    if problems:
        say("Needs a decision:")
        for p in problems:
            say("  - %s" % p)
    if apply and written:
        say("Result: written. %s" % ("Needs a decision (above)." if problems else "Clean."))
    elif apply:
        say("Result: nothing written.")
    else:
        files = sum(1 for e in entries if e.write is not None)
        failed = sum(1 for e in entries for o in e.outcomes if o.status == "failed")
        say("Result: %d file(s) would be written, %d hunk(s) do not apply, %d blocker(s). Nothing was written."
            % (files, failed, len(blockers)))
    return 1 if problems else 0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Sync ARMSX2's copy of PenguinScreen2's VR layer (a dry run unless "
                                             "--apply). See the comment at the top of this file.")
    ap.add_argument("--ps2", required=True, help="PenguinScreen2 checkout (only its git objects are read)")
    ap.add_argument("--commit", help="PenguinScreen2 commit or branch to sync to")
    ap.add_argument("--apply", action="store_true", help="write the files, the commit file, VR-SOURCE.md and the patches")
    ap.add_argument("--record", action="store_true",
                    help="write the patch files from ARMSX2's copies against PENGUINSCREEN2_COMMIT, and nothing else")
    ap.add_argument("--dry-run", action="store_true", help="with --record: only list what it would write")
    ap.add_argument("--branch", help="branch name for VR-SOURCE.md when it cannot be worked out")
    ap.add_argument("--fuzz", type=int, default=0, help="lines of context a hunk may lose at each end to apply (default 0)")
    ap.add_argument("--no-parity", action="store_true", help="do not run hook-parity.py")
    ap.add_argument("--armsx2", default=os.path.normpath(os.path.join(here, "..", "..")))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.record and (args.apply or args.commit):
        ap.error("--record works on PENGUINSCREEN2_COMMIT: it takes neither --apply nor --commit")
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    try:
        return run(args)
    except ToolError as e:
        print("sync-vr-layer: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
