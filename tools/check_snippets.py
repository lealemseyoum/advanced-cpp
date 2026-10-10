#!/usr/bin/env python3
"""
check_snippets.py -- compile / run the code blocks embedded in the course.

Every ```cpp block whose first line is a test header is verified:

    // @test <mode> [flags...] [cxx=COMPILER] [link=-lfoo,-pthread] [err=REGEX] [timeout=SECONDS]

Modes
    run     compile, link, run; the program must exit with status 0
    compile compile only (-c); must succeed
    fail    compilation must FAIL (use err=REGEX to check the diagnostic)
    crash   compile, run; the program must exit non-zero (sanitizer / abort demos)
    asm     compile with -S and capture the assembly (filter=sub1,sub2 keeps only
            functions whose demangled label contains one of the substrings)
    skip    not tested (the rest of the line is the reason)

Output blocks
    A ```text block that directly follows a `run` / `crash` block and starts with
    `# output` is rewritten by `--update` with the real program output.
    A ```asm block that directly follows an `asm` block and starts with `; asm`
    is rewritten with the real assembly.

Usage
    tools/check_snippets.py                       # check everything
    tools/check_snippets.py part-03*/06*.md       # check some files
    tools/check_snippets.py --update FILE...      # also refresh output/asm blocks
    tools/check_snippets.py --cxx g++-13 FILE...  # override the default compiler
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field

ROOT = pathlib.Path(__file__).resolve().parent.parent
OPEN = re.compile(r"^```(\S*)\s*$")
CLOSE = re.compile(r"^```\s*$")
HEADER = re.compile(r"^//\s*@test\s+(\w+)\s*(.*)$")
MAX_OUT_LINES = 70
FOLD = re.compile(r"^\s*</?(details|summary)\b.*$")  # collapsible wrappers may sit between a snippet and its output


@dataclass
class Block:
    lang: str
    start: int  # index of opening fence line
    end: int  # index of closing fence line
    body: list[str]


@dataclass
class Test:
    file: pathlib.Path
    block: Block
    mode: str
    flags: list[str] = field(default_factory=list)
    cxx: str | None = None
    link: list[str] = field(default_factory=list)
    err: str | None = None
    timeout: int = 30
    filt: list[str] = field(default_factory=list)
    lines: int = 0
    reason: str = ""
    out_block: Block | None = None


def parse_blocks(lines: list[str]) -> list[Block]:
    blocks, i = [], 0
    while i < len(lines):
        m = OPEN.match(lines[i])
        if m and m.group(1):
            j = i + 1
            while j < len(lines) and not CLOSE.match(lines[j]):
                j += 1
            blocks.append(Block(m.group(1), i, j, lines[i + 1 : j]))
            i = j + 1
        elif m:  # bare fence that is an opener (no language)
            j = i + 1
            while j < len(lines) and not CLOSE.match(lines[j]):
                j += 1
            blocks.append(Block("", i, j, lines[i + 1 : j]))
            i = j + 1
        else:
            i += 1
    return blocks


def collect(path: pathlib.Path) -> list[Test]:
    lines = path.read_text().split("\n")
    blocks = parse_blocks(lines)
    tests = []
    for idx, b in enumerate(blocks):
        if b.lang not in ("cpp", "c++", "c") or not b.body:
            continue
        m = HEADER.match(b.body[0])
        if not m:
            continue
        mode, rest = m.group(1), m.group(2).strip()
        t = Test(path, b, mode)
        if mode == "skip":
            t.reason = rest
            tests.append(t)
            continue
        for tok in rest.split():
            if tok.startswith("cxx="):
                t.cxx = tok[4:]
            elif tok.startswith("link="):
                t.link += [x for x in tok[5:].split(",") if x]
            elif tok.startswith("err="):
                t.err = tok[4:]
            elif tok.startswith("timeout="):
                t.timeout = int(tok[8:])
            elif tok.startswith("lines="):
                t.lines = int(tok[6:])
            elif tok.startswith("filter="):
                t.filt = [x for x in tok[7:].split(",") if x]
            else:
                t.flags.append(tok)
        # find paired output block
        if idx + 1 < len(blocks):
            nb = blocks[idx + 1]
            between = lines[b.end + 1 : nb.start]
            if all(not s.strip() or FOLD.match(s) for s in between) and nb.body:
                head = nb.body[0]
                if mode in ("run", "crash") and nb.lang == "text" and head.startswith("# output"):
                    t.out_block = nb
                if mode == "asm" and nb.lang == "asm" and head.startswith("; asm"):
                    t.out_block = nb
        tests.append(t)
    return tests


def pick_cxx(name: str | None, default: str) -> str:
    cxx = name or default
    if shutil.which(cxx):
        return cxx
    for fallback in ("g++-14", "g++", "clang++"):
        if shutil.which(fallback):
            return fallback
    raise SystemExit("no C++ compiler found")


def run(cmd, cwd, timeout):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        return 124, e.stdout or "", (e.stderr or "") + "\n[timeout]"


def clean_asm(text: str, filt: list[str]) -> str:
    dem = subprocess.run(["c++filt"], input=text, capture_output=True, text=True).stdout
    funcs, cur = [], None
    for ln in dem.split("\n"):
        s = ln.strip()
        if not s or s.startswith(("#", ";")):
            continue
        is_label = (not ln[0].isspace()) and s.endswith(":")
        if is_label:
            if s.startswith(".LC"):  # string / constant pool labels
                cur = None
                continue
            if s.startswith(".L"):  # real jump targets (.L2:) stay; CFI/debug labels (.LFB0:) go
                if cur is not None and re.match(r"^\.L\d+:$", s):
                    cur.append(ln)
                continue
            cur = [ln]
            funcs.append(cur)
            continue
        if s.startswith("."):  # assembler directive
            continue
        if cur is not None:
            cur.append(ln)
    keep = []
    for f in funcs:
        label = f[0]
        if len(f) == 1:  # a data label (no instructions): not a function
            continue
        if filt and not any(x in label for x in filt):
            continue
        keep.append("\n".join(f))
    return "\n\n".join(keep)


def execute(t: Test, default_cxx: str, update: bool):
    """Returns (ok, message, captured_text)."""
    if t.mode == "skip":
        return True, f"skip ({t.reason})", None
    cxx = pick_cxx(t.cxx, default_cxx)
    flags = list(t.flags)
    if not any(f.startswith("-std=") for f in flags):
        flags.append("-std=c++23")
    flags += ["-Wall", "-Wextra"]
    src_body = "\n".join(t.block.body) + "\n"
    with tempfile.TemporaryDirectory(prefix="snip-") as d:
        d = pathlib.Path(d)
        (d / "snippet.cpp").write_text(src_body)
        if t.mode == "compile":
            rc, out, err = run([cxx, *flags, "-c", "snippet.cpp", "-o", "/dev/null"], d, 120)
            return rc == 0, err.strip()[-1500:] or "ok", None
        if t.mode == "fail":
            rc, out, err = run([cxx, *flags, "-c", "snippet.cpp", "-o", "/dev/null"], d, 120)
            if rc == 0:
                return False, "expected a compile error but it compiled", None
            if t.err and not re.search(t.err, err):
                return False, f"error text did not match /{t.err}/:\n{err[-1200:]}", None
            return True, "failed to compile, as intended", None
        if t.mode == "asm":
            rc, out, err = run(
                [cxx, *flags, "-S", "-masm=intel", "-fno-asynchronous-unwind-tables",
                 "-fno-ident", "-fcf-protection=none", "snippet.cpp", "-o", "snippet.s"],
                d, 120)
            if rc != 0:
                return False, err.strip()[-1500:], None
            asm = clean_asm((d / "snippet.s").read_text(), t.filt)
            return True, "ok", asm
        # run / crash
        rc, out, err = run([cxx, *flags, "snippet.cpp", "-o", "snippet", *t.link], d, 180)
        if rc != 0:
            return False, "compile failed:\n" + err.strip()[-1800:], None
        rc, out, err = run(["./snippet"], d, t.timeout)
        if t.mode == "run":
            if rc != 0:
                return False, f"exit status {rc}\n{err[-800:]}", None
            return True, "ok", out
        if t.mode == "crash":
            if rc == 0:
                return False, "expected a non-zero exit status", None
            if t.err and not re.search(t.err, out + err):
                return False, f"output did not match /{t.err}/:\n{(out + err)[-1200:]}", None
            return True, f"exited with {rc}, as intended", normalise_crash(out + err)
    return False, f"unknown mode {t.mode}", None


def normalise_crash(text: str) -> str:
    """Make sanitizer / abort output reproducible and readable: strip temp paths, addresses, pids,
    build ids, binary offsets, libc start-up frames, ASan shadow dumps and libstdc++ internal frames."""
    keep: list[str] = []
    in_shadow = False
    elided = False
    for line in text.split("\n"):
        if line.startswith("Shadow bytes around the buggy address"):
            in_shadow = True
            keep.append("[ASan shadow-memory dump and legend omitted]")
            continue
        if in_shadow:
            if line.startswith("==") and "ABORTING" in line:
                in_shadow = False
                keep.append(re.sub(r"==\d+==", "==PID==", line))
            continue
        if re.search(r"__libc_start|libc-start|\b_start \(", line):
            continue
        if re.match(r"^ ([0-9a-f]{2} ?)+\s*$", line) or re.match(r"^\s+\^~*\s*$", line):
            continue                                   # UBSan raw memory dumps contain stack garbage
        if re.match(r"^\s+#\d+ ", line) and "/usr/include/c++/" in line and "snippet.cpp" not in line:
            if not elided:
                keep.append("    [... libstdc++ internal frames elided ...]")
                elided = True
            continue
        if not re.match(r"^\s+#\d+ ", line):
            elided = False
        line = re.sub(r" \(BuildId: [0-9a-f]+\)", "", line)
        line = re.sub(r" \((snippet|lib[\w.+-]*)\+0x[0-9a-f]+\)", "", line)
        line = re.sub(r"/tmp/snip-[^/]+/", "", line)
        line = re.sub(r"==\d+==", "==PID==", line)
        line = re.sub(r"\(pid=\d+\)", "(pid=N)", line)
        line = re.sub(r"\(tid=\d+, ", "(tid=N, ", line)
        line = re.sub(r"0x[0-9a-f]{6,}", "0xADDR", line)
        line = re.sub(r"\+0x[0-9a-f]{2,6}\)", "+0x…)", line)
        keep.append(line)
    return "\n".join(keep)


def version_of(cxx: str) -> str:
    p = subprocess.run([cxx, "--version"], capture_output=True, text=True)
    first = p.stdout.split("\n")[0]
    m = re.search(r"(\d+\.\d+\.\d+)", first)
    name = "clang" if "clang" in first else "gcc"
    return f"{name} {m.group(1)}" if m else cxx


def trim(text: str, limit: int = 0) -> list[str]:
    limit = limit or MAX_OUT_LINES
    lines = text.rstrip("\n").split("\n")
    if len(lines) > limit:
        lines = lines[:limit] + ["... (truncated)"]
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--update", action="store_true", help="rewrite output/asm blocks with real results")
    ap.add_argument("--cxx", default="g++-14", help="default compiler (default: g++-14)")
    ap.add_argument("-j", "--jobs", type=int, default=2)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    files = []
    for arg in args.files or [str(ROOT)]:
        path = pathlib.Path(arg).resolve()
        files += sorted(path.rglob("*.md")) if path.is_dir() else [path]
    files = [f for f in files if ".git" not in f.parts]
    all_tests = []
    for f in files:
        all_tests += collect(f)
    if not all_tests:
        print("no tagged snippets found")
        return 0

    failures = 0
    results = {}
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(execute, t, args.cxx, args.update): t for t in all_tests}
        for fut in cf.as_completed(futs):
            results[id(futs[fut])] = fut.result()

    counts = {"ok": 0, "skip": 0, "fail": 0}
    for t in all_tests:
        ok, msg, captured = results[id(t)]
        try:
            rel = t.file.relative_to(ROOT)
        except ValueError:
            rel = t.file
        where = f"{rel}:{t.block.start + 1}"
        if t.mode == "skip":
            counts["skip"] += 1
            if args.verbose:
                print(f"SKIP  {where}  {msg}")
        elif ok:
            counts["ok"] += 1
            if args.verbose:
                print(f"ok    {where}  [{t.mode}]")
        else:
            counts["fail"] += 1
            failures += 1
            print(f"FAIL  {where}  [{t.mode}]\n{msg}\n")

    if args.update:
        by_file: dict[pathlib.Path, list[Test]] = {}
        for t in all_tests:
            ok, msg, captured = results[id(t)]
            if ok and captured is not None and t.out_block is not None:
                by_file.setdefault(t.file, []).append(t)
        for f, ts in by_file.items():
            lines = f.read_text().split("\n")
            for t in sorted(ts, key=lambda x: -x.out_block.start):
                ok, msg, captured = results[id(t)]
                cxx = pick_cxx(t.cxx, args.cxx)
                ver = version_of(cxx)
                if t.mode == "asm":
                    head = f"; asm ({ver}, {' '.join(x for x in t.flags if x.startswith('-O')) or '-O0'}, x86-64, Intel syntax)"
                else:
                    head = f"# output ({ver}, x86-64 Linux)"
                body = [head] + trim(captured, t.lines)
                ob = t.out_block
                lines[ob.start + 1 : ob.end] = body
            f.write_text("\n".join(lines))
        print(f"updated output blocks in {len(by_file)} file(s)")

    print(f"\n{counts['ok']} ok, {counts['fail']} failed, {counts['skip']} skipped "
          f"({len(all_tests)} tagged snippets in {len(files)} files)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
