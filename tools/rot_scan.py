"""Static rot / Power-of-10 scan for this repository.

Reports: functions longer than 70 lines, functions of 30+ lines without an
assertion, loops with no visible bound, TODO/FIXME/HACK markers, header
declarations referenced nowhere else, and Win32/SQLite calls whose result
is dropped. Run from anywhere: ``python tools/rot_scan.py``.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LONG_FUNCTION = 70
ASSERT_FREE_MIN = 30
MAX_FILES = 10000

SIG = re.compile(
    r"^[A-Za-z_][\w:<>,\s\*&\[\]]*\b([A-Za-z_]\w*(?:::[A-Za-z_~]\w*)*)\s*\([^;]*\)\s*(const\s*)?\{\s*$"
)
DECL = re.compile(r"^\s*(?:[\w:<>,\*&\s]+?)\b([a-z_][a-z0-9_]*)\s*\([^;{]*\)\s*(?:const)?\s*;", re.MULTILINE)
DROPPED = re.compile(
    r"(WinHttpSetOption|WinHttpSetTimeouts|sqlite3_bind_\w+|CryptBinaryToStringA|SetWindowSubclass|RegisterClassExW)\("
)
KEYWORDS = ("if", "for", "while", "switch", "else", "#", "//", "return")


def tracked_files() -> list[str]:
    out = subprocess.check_output(["git", "ls-files"], cwd=ROOT, text=True).split()
    assert len(out) <= MAX_FILES
    return out


def read_all(files: list[str]) -> dict[str, str]:
    texts: dict[str, str] = {}
    for f in files:
        if f.endswith((".cpp", ".h", ".md", ".txt")):
            with open(os.path.join(ROOT, f), encoding="utf-8") as fh:
                texts[f] = fh.read()
    return texts


def function_spans(lines: list[str]) -> list[tuple[int, int, str]]:
    """(start, end, name) for each function definition, by brace matching."""
    spans: list[tuple[int, int, str]] = []
    i = 0
    while i < len(lines):
        m = SIG.match(lines[i])
        if m and not lines[i].lstrip().startswith(KEYWORDS):
            depth = 0
            j = i
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth <= 0 and j > i:
                    break
                j += 1
            spans.append((i, j, m.group(1)))
            i = j + 1
        else:
            i += 1
    return spans


def main() -> int:
    files = tracked_files()
    texts = read_all(files)
    cpp = [f for f in files if f.endswith((".cpp", ".h")) and not f.startswith("tests/")]

    long_funcs: list[tuple[int, str, int, str]] = []
    assert_free: list[tuple[int, str, int, str]] = []
    for f in cpp:
        lines = texts[f].split("\n")
        for start, end, name in function_spans(lines):
            body = "\n".join(lines[start : end + 1])
            n = end - start + 1
            asserts = len(re.findall(r"G_ASSERT|G_REQUIRE_RET|G_REQUIRE_VOID", body))
            if n > LONG_FUNCTION:
                long_funcs.append((n, f, start + 1, name))
            if n >= ASSERT_FREE_MIN and asserts == 0:
                assert_free.append((n, f, start + 1, name))

    print(f"== functions longer than {LONG_FUNCTION} lines")
    for n, f, ln, name in sorted(long_funcs, reverse=True):
        print(f"{n:4d}  {f}:{ln}  {name}")

    print(f"\n== functions with >= {ASSERT_FREE_MIN} lines and no assertion/require")
    for n, f, ln, name in sorted(assert_free, reverse=True):
        print(f"{n:4d}  {f}:{ln}  {name}")

    print("\n== suspicious loops")
    for f in cpp:
        for ln, line in enumerate(texts[f].split("\n"), 1):
            s = line.strip()
            if re.match(r"(for\s*\(\s*;\s*;\s*\)|while\s*\(\s*(true|1)\s*\))", s):
                print(f"{f}:{ln}  {s}")

    print("\n== TODO / FIXME / HACK / XXX")
    for f, t in texts.items():
        if f.endswith("rot_scan.py"):
            continue
        for ln, line in enumerate(t.split("\n"), 1):
            if re.search(r"\b(TODO|FIXME|HACK|XXX)\b", line):
                print(f"{f}:{ln}  {line.strip()[:100]}")

    print("\n== declared in a header but referenced nowhere else")
    all_code = "\n".join(t for f, t in texts.items() if f.endswith((".cpp", ".h")))
    for f in cpp:
        if not f.endswith(".h"):
            continue
        for m in DECL.finditer(texts[f]):
            name = m.group(1)
            if name in ("operator", "if", "return", "sizeof", "abort"):
                continue
            uses = len(re.findall(r"\b" + re.escape(name) + r"\b", all_code))
            if uses <= 2:  # declaration + definition only
                print(f"{f}: {name}()  ({uses} mentions)")

    print("\n== calls whose result is dropped")
    for f in cpp:
        for ln, line in enumerate(texts[f].split("\n"), 1):
            s = line.strip()
            if DROPPED.match(s):
                print(f"{f}:{ln}  {s[:90]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
