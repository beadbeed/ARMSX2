#!/usr/bin/env python3
# Outbreak VR (Steam Frame): checks sync-vr-layer.py on small made-up repos, with no real PenguinScreen2 or ARMSX2.
#
# Builds a git repo that plays PenguinScreen2 (its files have CRLF line endings, as the real ones do) and a folder
# that plays ARMSX2 (LF, one file CRLF), with local edits in the copies, then runs the tool's command line:
# --record, a dry run with nothing changed, a dry run against a commit that moves, conflicts with and already
# has some of the edits, adds a file and removes one, an unrecorded edit (a sync must refuse), --apply with a
# conflict (the .rej file, the partly applied file, the commit file, the doc), the manual fix and --record, and a
# clean --apply with its self-check. hook-parity is not run (the made-up ARMSX2 has no shared files).
#
# Usage: python3 tools/outbreak-vr/sync-vr-layer-test.py     Exit: 0 passed, 1 failed. Needs git.

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "sync-vr-layer.py")
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def write(base, rel, text, crlf=False):
    path = os.path.join(base, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = text.replace("\n", "\r\n") if crlf else text
    with open(path, "wb") as f:
        f.write(data.encode("utf-8") if isinstance(data, str) else data)


def read(base, rel):
    with open(os.path.join(base, *rel.split("/")), "rb") as f:
        return f.read()


def exists(base, rel):
    return os.path.exists(os.path.join(base, *rel.split("/")))


def git(repo, *args):
    p = subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@example.com",
                        "-c", "core.autocrlf=false"] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), p.stderr.decode()))
    return p.stdout.decode().strip()


def run(ps2, root, *args):
    p = subprocess.run([sys.executable, TOOL, "--ps2", ps2, "--armsx2", root, "--no-parity"] + list(args),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def snapshot(root):
    snap = {}
    for folder, _, files in os.walk(root):
        for name in files:
            full = os.path.join(folder, name)
            with open(full, "rb") as f:
                snap[os.path.relpath(full, root)] = f.read()
    return snap


def numbered(prefix, n):
    return "".join("%s%d\n" % (prefix, i) for i in range(1, n + 1))


def lines_of(text):
    return text.replace("\r\n", "\n").split("\n")


def unit_checks():
    spec = importlib.util.spec_from_file_location("sync_vr_layer", TOOL)
    sync = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sync)
    nl = "\n"
    base = [w + nl for w in "a b c d e f g h i j".split()]
    edited = list(base)
    edited[4] = "E-armsx2" + nl
    hunks = sync.make_hunks(base, edited)
    check(len(hunks) == 1, "unit: one edit is one hunk")
    # upstream changed the outermost context line above the edit: fails as it is, applies with fuzz 1
    upstream = list(base)
    upstream[1] = "B-upstream" + nl
    result, outs = sync.apply_hunks(upstream, hunks, 0)
    check(outs[0].status == "failed" and result == upstream, "unit: a hunk whose context changed does not apply (fuzz 0)")
    result, outs = sync.apply_hunks(upstream, hunks, 1)
    check(outs[0].status == "applied" and outs[0].fuzz == 1 and result[4] == "E-armsx2" + nl and result[1] == "B-upstream" + nl,
          "unit: with fuzz 1 it applies and says so")
    # the edit is in the new copy already
    result, outs = sync.apply_hunks(edited, hunks, 0)
    check(outs[0].status == "already" and result == edited, "unit: an edit that is already there is skipped")
    # a patch that is only an insertion, written against code that has since moved down
    ins = base[:5] + ["new" + nl] + base[5:]
    h2 = sync.make_hunks(base, ins)
    shifted = ["x" + nl] * 7 + base
    result, outs = sync.apply_hunks(shifted, h2, 0)
    check(outs[0].status == "applied" and outs[0].offset == 7 and result == ["x" + nl] * 7 + ins,
          "unit: an insertion follows the code it was written against")
    parsed = sync.parse_patch(sync.format_patch("f", hunks), "f")
    check([h.lines for h in parsed] == [h.lines for h in hunks], "unit: format_patch and parse_patch round-trip")


def main():
    unit_checks()
    tmp = tempfile.mkdtemp(prefix="sync-vr-layer-test-")
    try:
        ps2 = os.path.join(tmp, "ps2")
        root = os.path.join(tmp, "armsx2")
        os.makedirs(ps2)
        git(ps2, "init", "-q", "-b", "main")

        # ---- PenguinScreen2 at C1 (CRLF)
        alpha = numbered("int a", 40)
        beta = numbered("int b", 20)
        gamma = numbered("int g", 20)
        eof = numbered("int e", 12).rstrip("\n")           # no newline at the end
        plain = numbered("int p", 8)
        for rel, text in (("pcsx2/VR/Alpha.cpp", alpha), ("pcsx2/VR/Beta.h", beta), ("pcsx2/VR/Gamma.cpp", gamma),
                          ("pcsx2/VR/Eof.h", eof), ("pcsx2/VR/Plain.cpp", plain), ("pcsx2/VR/Epsilon.cpp", "int eps;\n"),
                          ("bin/resources/vr-profiles/G.yaml", "name: g\nsize: 1\n"),
                          ("pcsx2/VR/StereoSettings.cpp", "// not in PenguinScreen2\n")):
            write(ps2, rel, text, crlf=True)
        write(ps2, "bin/resources/vr-profiles/blob.bin", b"\x00\x01\x02\x03")
        os.remove(os.path.join(ps2, "pcsx2", "VR", "StereoSettings.cpp"))
        write(ps2, "pcsx2/Config.h", "struct VROptions { bool EdgeFill = false; };\n", crlf=True)
        git(ps2, "add", "-A")
        git(ps2, "commit", "-q", "-m", "C1 the copied version")
        c1 = git(ps2, "rev-parse", "HEAD")

        # ---- ARMSX2: LF copies with local edits (Beta.h stays CRLF)
        def edit(text, old, new):
            assert old in text
            return text.replace(old, new, 1)

        a_local = edit(edit(alpha, "int a5\n", "int a5\n// ARMSX2 edit A1\n"), "int a35\n", "int a35_armsx2\n")
        b_local = edit(beta, "int b10\n", "int b10_armsx2\n")
        g_local = edit(gamma, "int g8\n", "int g8\n// ported back\n")
        e_local = edit(eof, "int e12", "int e12_armsx2")
        write(root, "pcsx2/VR/Alpha.cpp", a_local)
        write(root, "pcsx2/VR/Beta.h", b_local, crlf=True)
        write(root, "pcsx2/VR/Gamma.cpp", g_local)
        write(root, "pcsx2/VR/Eof.h", e_local)
        write(root, "pcsx2/VR/Plain.cpp", plain)
        write(root, "pcsx2/VR/Epsilon.cpp", "int eps;\n")
        write(root, "pcsx2/VR/StereoSettings.cpp", "// ARMSX2's own\n")
        write(root, "bin/resources/vr-profiles/G.yaml", "name: g\nsize: 1\n")
        write(root, "bin/resources/vr-profiles/blob.bin", b"\x00\x01\x02\x03")
        write(root, "pcsx2/VR/VR-SOURCE.md",
              "# Where pcsx2/VR comes from\n\nThe layer is copied from **PenguinScreen2** (https://example.invalid),\n"
              "branch `main`, commit **%s** (2026-01-01, \"C1 the copied\nversion\"). It is built only when on.\n\n"
              "More text that stays as it is.\n" % c1)
        write(root, "OUTBREAK-VR.md", "copied from PenguinScreen2 %s\n" % c1[:7])
        write(root, "tools/outbreak-vr/PENGUINSCREEN2_COMMIT", c1 + "\n")

        # ---- 1. --record
        code, out = run(ps2, root, "--record")
        check(code == 0, "--record exits 0")
        for rel in ("Alpha.cpp", "Beta.h", "Gamma.cpp", "Eof.h"):
            check(exists(root, "tools/outbreak-vr/vr-local-edits/pcsx2/VR/%s.patch" % rel), "--record writes a patch for %s" % rel)
        for rel in ("pcsx2/VR/Plain.cpp", "pcsx2/VR/Epsilon.cpp", "bin/resources/vr-profiles/G.yaml",
                    "bin/resources/vr-profiles/blob.bin", "pcsx2/VR/StereoSettings.cpp"):
            check(not exists(root, "tools/outbreak-vr/vr-local-edits/%s.patch" % rel), "no patch for %s (no local edit, or not copied)" % rel)
        patch_alpha = read(root, "tools/outbreak-vr/vr-local-edits/pcsx2/VR/Alpha.cpp.patch").decode()
        check(b"\r" not in patch_alpha.encode() and "@@" in patch_alpha and "ARMSX2 edit A1" in patch_alpha,
              "a patch is LF text with the edit in it")
        code, out = run(ps2, root, "--record", "-v")
        check(code == 0 and out.count("unchanged") >= 4 and "written" not in out, "a second --record changes nothing")
        # the patch is a real unified diff: git apply produces ARMSX2's copy from the LF text of PenguinScreen2's
        work = os.path.join(tmp, "applycheck")
        write(work, "pcsx2/VR/Alpha.cpp", alpha)
        p = subprocess.run(["git", "-C", work, "-c", "core.autocrlf=false", "apply", "--whitespace=nowarn",
                            os.path.join(root, "tools", "outbreak-vr", "vr-local-edits", "pcsx2", "VR", "Alpha.cpp.patch")],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(p.returncode == 0 and read(work, "pcsx2/VR/Alpha.cpp").decode() == a_local, "git apply turns the LF copy into ARMSX2's")

        # ---- 2. a dry run against the recorded commit
        before = snapshot(root)
        code, out = run(ps2, root, "--commit", c1)
        check(code == 0, "dry run against the recorded commit exits 0")
        check("Nothing: every one is the same" in out or "nothing, every one is the same" in out, "it lists no change")
        check("4 of 4 patches apply cleanly" in out, "all 4 patches apply")
        check(snapshot(root) == before, "a dry run writes nothing")

        # ---- PenguinScreen2 at C2
        write(ps2, "pcsx2/VR/Alpha.cpp", "// new first line\n// and another\n// and a third\n" +
              edit(alpha, "int a20\n", "int a20_upstream\n"), crlf=True)
        write(ps2, "pcsx2/VR/Beta.h", edit(beta, "int b10\n", "int b10_upstream\n"), crlf=True)
        write(ps2, "pcsx2/VR/Gamma.cpp", edit(gamma, "int g8\n", "int g8\n// ported back\n"), crlf=True)
        write(ps2, "pcsx2/VR/Eof.h", edit(eof, "int e1\n", "int e1_upstream\n"), crlf=True)
        write(ps2, "pcsx2/VR/Delta.cpp", "int delta;\n", crlf=True)
        os.remove(os.path.join(ps2, "pcsx2", "VR", "Epsilon.cpp"))
        write(ps2, "bin/resources/vr-profiles/G.yaml", "name: g\nsize: 2\n", crlf=True)
        write(ps2, "bin/resources/vr-profiles/blob.bin", b"\x00\x09\x09\x09\x09")
        write(ps2, "pcsx2/Config.h", "struct VROptions { bool EdgeFill = true; };\n", crlf=True)
        git(ps2, "add", "-A")
        git(ps2, "commit", "-q", "-m", "C2 moves things")
        git(ps2, "branch", "feature")

        # ---- 3. a dry run against C2
        before = snapshot(root)
        code, out = run(ps2, root, "--commit", "feature")
        check(code == 1, "dry run against C2 exits 1 (a hunk does not apply)")
        check("pcsx2/VR/Beta.h" in out and "1 of 1 hunks DO NOT APPLY" in out and "DOES NOT APPLY" in out,
              "it reports the Beta.h hunk that does not apply")
        check("closest place" in out, "it says where the failed hunk comes closest")
        check("already in the new PenguinScreen2 copy" in out, "it sees Gamma.cpp's edit is already upstream")
        check("moved by +3" in out, "it reports Alpha.cpp's hunks moved by the 3 new lines")
        check("added" in out and "pcsx2/VR/Delta.cpp" in out, "it lists the new file")
        check("removed in PenguinScreen2" in out and "Epsilon.cpp" in out, "it lists the removed file and leaves it")
        check("pcsx2/Config.h" in out, "it lists the hand-ported file that changed")
        check("StereoSettings" not in out, "it never touches the ARMSX2-only file")
        check(snapshot(root) == before, "that dry run wrote nothing")

        # ---- 4. an unrecorded edit blocks --apply
        write(root, "pcsx2/VR/Plain.cpp", plain + "// an edit nobody recorded\n")
        before = snapshot(root)
        code, out = run(ps2, root, "--commit", "feature", "--apply")
        check(code == 1 and "BLOCKER" in out and "not recorded" in out, "--apply refuses a copy with an unrecorded edit")
        check(snapshot(root) == before, "and writes nothing")
        write(root, "pcsx2/VR/Plain.cpp", plain)

        # ---- 5. --apply with a conflict
        code, out = run(ps2, root, "--commit", "feature", "--apply")
        check(code == 1, "--apply with a conflict exits 1")
        alpha_now = lines_of(read(root, "pcsx2/VR/Alpha.cpp").decode())
        check(alpha_now[:3] == ["// new first line", "// and another", "// and a third"] and "int a20_upstream" in alpha_now
              and "// ARMSX2 edit A1" in alpha_now and "int a35_armsx2" in alpha_now, "Alpha.cpp: upstream's changes and ARMSX2's edits are both there")
        check(b"\r" not in read(root, "pcsx2/VR/Alpha.cpp"), "Alpha.cpp stays LF")
        beta_now = read(root, "pcsx2/VR/Beta.h")
        check(b"int b10_upstream" in beta_now and b"int b10_armsx2" not in beta_now, "Beta.h has upstream's line (the edit did not apply)")
        check(beta_now.count(b"\r\n") == beta_now.count(b"\n"), "Beta.h stays CRLF")
        check(exists(root, "pcsx2/VR/Beta.h.rej") and b"int b10_armsx2" in read(root, "pcsx2/VR/Beta.h.rej"), "Beta.h.rej holds the failed hunk")
        gamma_now = read(root, "pcsx2/VR/Gamma.cpp").decode()
        check(gamma_now.count("// ported back") == 1, "Gamma.cpp's edit is not applied twice")
        eof_now = read(root, "pcsx2/VR/Eof.h")
        check(eof_now.endswith(b"int e12_armsx2") and b"int e1_upstream" in eof_now, "Eof.h keeps having no newline at the end, with both changes")
        check(read(root, "pcsx2/VR/Delta.cpp") == b"int delta;\n", "the new file is created (LF)")
        check(exists(root, "pcsx2/VR/Epsilon.cpp"), "the removed file is left in place")
        check(read(root, "bin/resources/vr-profiles/blob.bin") == b"\x00\x09\x09\x09\x09", "a binary file is copied as it is")
        check(read(root, "bin/resources/vr-profiles/G.yaml") == b"name: g\nsize: 2\n", "a profile is copied (LF)")
        check(read(root, "pcsx2/VR/StereoSettings.cpp") == b"// ARMSX2's own\n", "ARMSX2's own file is untouched")
        c2 = git(ps2, "rev-parse", "HEAD")
        check(read(root, "tools/outbreak-vr/PENGUINSCREEN2_COMMIT").decode().strip() == c2, "the commit file holds C2")
        doc = read(root, "pcsx2/VR/VR-SOURCE.md").decode()
        check("branch `feature`" in " ".join(doc.split()) and c2 in doc and c1 not in doc and "More text that stays as it is." in doc,
              "the source line names the branch and C2; the rest of the doc stays")
        check("Written by" in read(root, "tools/outbreak-vr/vr-local-edits/pcsx2/VR/Alpha.cpp.patch").decode()
              and b"@@ -3," in read(root, "tools/outbreak-vr/vr-local-edits/pcsx2/VR/Alpha.cpp.patch"),
              "the patches are not re-recorded while a hunk is open")
        code, out = run(ps2, root, "--record")
        check(code == 2 and "Beta.h.rej" in out, "--record refuses while a .rej file is there")
        code, out = run(ps2, root, "--commit", "feature", "--apply")
        check(code == 1 and "NOT APPLIED" in out, "--apply refuses while a .rej file is there (or while Beta.h's patch is stale)")

        # ---- the manual fix: make Beta.h's edit by hand, delete the .rej, --record
        beta_fixed = edit(beta, "int b10\n", "int b10_upstream\n// fixed by hand\n")
        write(root, "pcsx2/VR/Beta.h", beta_fixed, crlf=True)
        os.remove(os.path.join(root, "pcsx2", "VR", "Beta.h.rej"))
        code, out = run(ps2, root, "--record")
        check(code == 0, "--record after the manual fix exits 0")
        code, out = run(ps2, root, "--commit", "feature")
        check(code == 0 and "0 file(s) would be written" in out, "then a dry run against C2 shows no changes and exits 0")
        check(not exists(root, "tools/outbreak-vr/vr-local-edits/pcsx2/VR/Gamma.cpp.patch"),
              "Gamma.cpp's patch is dropped by --record: its edit is upstream now")

        # ---- 6. a clean --apply
        write(ps2, "pcsx2/VR/Alpha.cpp", read(ps2, "pcsx2/VR/Alpha.cpp").decode().replace("int a30\r\n", "int a30_c3\r\n"), crlf=False)
        write(ps2, "pcsx2/VR/Plain.cpp", plain + "int p9;\n", crlf=True)
        git(ps2, "add", "-A")
        git(ps2, "commit", "-q", "-m", "C3 small changes")
        c3 = git(ps2, "rev-parse", "HEAD")
        code, out = run(ps2, root, "--commit", c3, "--apply")
        check(code == 0, "a clean --apply exits 0")
        if code != 0:
            print(out)
        check("Self-check: a dry run against the new commit shows no changes" in out, "its self-check passes")
        check(b"int a30_c3" in read(root, "pcsx2/VR/Alpha.cpp") and b"// ARMSX2 edit A1" in read(root, "pcsx2/VR/Alpha.cpp"),
              "Alpha.cpp has C3's change and the edits")
        check(read(root, "pcsx2/VR/Plain.cpp") == (plain + "int p9;\n").encode(), "Plain.cpp is updated (LF)")
        check(read(root, "tools/outbreak-vr/PENGUINSCREEN2_COMMIT").decode().strip() == c3, "the commit file holds C3")
        check("commit **%s**" % c3 in " ".join(read(root, "pcsx2/VR/VR-SOURCE.md").decode().split()), "the doc names C3 (its source line was found again after the first rewrap)")
        code, out = run(ps2, root, "--commit", c3)
        check(code == 0 and "0 file(s) would be written" in out, "a dry run against C3 afterwards shows no changes")

        # ---- bad input
        code, out = run(ps2, root, "--commit", "no-such-commit")
        check(code == 2, "an unknown commit exits 2")
        code, out = run(ps2, root)
        check(code == 2 and "--commit is required" in out, "no --commit exits 2")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d check(s) failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
