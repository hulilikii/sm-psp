#!/usr/bin/env python3
"""
PSP Allegrex alignment remediation for hulilikii/sm-psp.

Target baseline inspected: main @ 5db0692ed9e6f4b69b3a79f666d1a126d80e3988.
Run from the repository root (or place this script in the repository root):
    python3 psp_alignment_audit_fix.py --check
    python3 psp_alignment_audit_fix.py
    python3 psp_alignment_audit_fix.py --disasm smpsp.disasm.txt

This changes explicit 16/32-bit pointer dereferences to the project's
aligned(1), may_alias pointer typedefs. It also fixes two odd-offset halfword
loads identified in the supplied ELF disassembly. It is not proof that every
dynamic pointer is aligned or that the game will boot.
"""
from __future__ import annotations
import argparse
import pathlib
import re
import shutil
import subprocess
import sys

TARGET_MAIN = "5db0692ed9e6f4b69b3a79f666d1a126d80e3988"
EXTENSIONS = {".c", ".h"}

TYPE_RULES = (
    ("uint16_t", "unaligned_uint16"), ("int16_t", "unaligned_int16"),
    ("uint32_t", "unaligned_uint32"), ("int32_t", "unaligned_int32"),
    ("uint16", "unaligned_uint16"), ("int16", "unaligned_int16"),
    ("uint32", "unaligned_uint32"), ("int32", "unaligned_int32"),
)
DIRECT_RULES = []
for original, alias in TYPE_RULES:
    DIRECT_RULES.append((
        re.compile(r"\*\s*\(\s*const\s+" + original + r"\s*\*\s*\)"),
        "* (const " + alias + " *)"))
    DIRECT_RULES.append((
        re.compile(r"\*\s*\(\s*" + original + r"\s*\*\s*\)"),
        "* (" + alias + " *)"))

# Only change casts that are actually inside an indexed dereference:
# *((uint16 *)byte_ptr + 3). Do not change arbitrary function arguments.
INDEXED_RULES = []
for original, alias in TYPE_RULES:
    INDEXED_RULES.append((
        re.compile(r"\*\s*\(\s*\(\s*const\s+" + original +
                   r"\s*\*\s*\)\s*([A-Za-z_]\w*)\s*\+\s*"),
        "*((const " + alias + r" *)\1 + "))
    INDEXED_RULES.append((
        re.compile(r"\*\s*\(\s*\(\s*" + original +
                   r"\s*\*\s*\)\s*([A-Za-z_]\w*)\s*\+\s*"),
        "*((" + alias + r" *)\1 + "))

def transform(text: str) -> str:
    updated = text
    # VoidP is a uint16 typedef; this catches the odd ROM-table dereference
    # in DrawFileSelectMapIcons that corresponds to an odd lhu in the ELF.
    updated = re.sub(r"\*\s*\(\s*const\s+VoidP\s*\*\s*\)",
                     "* (const unaligned_uint16 *)", updated)
    updated = re.sub(r"\*\s*\(\s*VoidP\s*\*\s*\)",
                     "* (unaligned_uint16 *)", updated)

    for pattern, replacement in DIRECT_RULES:
        updated = pattern.sub(replacement, updated)
    for pattern, replacement in INDEXED_RULES:
        updated = pattern.sub(replacement, updated)

    # v2 and p are byte pointers into ROM. Keep their 16-bit accesses
    # explicitly alignment-1, including accesses via the local v5 pointer.
    updated = re.sub(
        r"\buint16\s*\*\s*v5\s*=\s*\(\s*uint16\s*\*\s*\)\s*v2\s*;",
        "const unaligned_uint16 *v5 = (const unaligned_uint16 *)v2;",
        updated)
    updated = re.sub(
        r"\bvoid\s+InitializeCorpseRottingDataTable\s*\(\s*uint16\s*\*\s*table\s*,",
        "void InitializeCorpseRottingDataTable(unaligned_uint16 *table,",
        updated)
    updated = re.sub(
        r"InitializeCorpseRottingDataTable\s*\(\s*\(\s*uint16\s*\*\s*\)\s*\(\s*g_ram\s*\+\s*\*v5\s*\)",
        "InitializeCorpseRottingDataTable((unaligned_uint16 *)(g_ram + *v5)",
        updated)
    return updated

def odd_halfword_instructions(path: pathlib.Path):
    # GNU objdump prints immediates in hex without a 0x prefix.
    pattern = re.compile(
        r"^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(lh|lhu|sh)\s+"
        r"([^,]+),\s*([0-9a-f]+)\((\w+)\)", re.I)
    findings = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        print(f"Cannot read disassembly {path}: {exc}", file=sys.stderr)
        return findings
    for number, line in enumerate(lines, 1):
        m = pattern.match(line)
        if m and (int(m.group(4), 16) & 1):
            findings.append((number, line.strip()))
    print(f"\nELF disassembly audit: {len(findings)} lh/lhu/sh instructions have odd immediates.")
    print("An odd immediate alone is not proof of an unaligned effective address;")
    print("the base register's runtime alignment must also be established.")
    for number, line in findings:
        print(f"  line {number}: {line}")
    return findings

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="show planned changes without writing files")
    parser.add_argument("--disasm", type=pathlib.Path,
                        help="report odd-immediate lh/lhu/sh instructions")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent
    src = root / "src"
    if not src.is_dir():
        print("ERROR: put this script in the sm-psp repository root.", file=sys.stderr)
        return 2
    types_h = src / "types.h"
    if not types_h.exists() or "unaligned_uint16" not in types_h.read_text(
            encoding="utf-8", errors="replace"):
        print("ERROR: src/types.h does not define unaligned_uint16; refusing to edit.",
              file=sys.stderr)
        return 2

    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                              check=True, capture_output=True, text=True).stdout.strip()
        if head != TARGET_MAIN:
            print(f"WARNING: HEAD is {head}, not inspected baseline {TARGET_MAIN}.")
            print("Review the resulting diff before building.")
    except (OSError, subprocess.CalledProcessError):
        print("NOTE: Git HEAD unavailable; using source files present in this directory.")

    changed = []
    for path in sorted(src.rglob("*")):
        if path.suffix not in EXTENSIONS:
            continue
        original = path.read_text(encoding="utf-8", errors="surrogateescape")
        updated = transform(original)
        if updated != original:
            changed.append((path, original, updated))

    if args.check:
        print(f"Check only: {len(changed)} file(s) would change.")
        for path, _, _ in changed:
            print("  " + str(path.relative_to(root)))
    else:
        for path, original, updated in changed:
            backup = path.with_suffix(path.suffix + ".pre-psp-align")
            if not backup.exists():
                shutil.copy2(path, backup)
            path.write_text(updated, encoding="utf-8", errors="surrogateescape")
        print(f"Updated {len(changed)} file(s). Backups: *.pre-psp-align (created only if absent).")
        print("Review with: git diff --check && git diff")

    if args.disasm:
        disasm = args.disasm if args.disasm.is_absolute() else root / args.disasm
        odd_halfword_instructions(disasm)

    print("\nRequired verification:")
    print("  1. Review the complete diff; do not commit automatically.")
    print("  2. Rebuild with the same PSPSDK/toolchain as the supplied ELF.")
    print("  3. Regenerate disassembly and compare lh/lhu/sh effective addresses.")
    print("  4. Test first in PPSSPP, then on PSP hardware.")
    print("This script does not modify GitHub or push commits.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
