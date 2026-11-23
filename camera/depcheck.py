#!/usr/bin/env python3
"""
Walk the whole dependency tree of an ELF library and report every undefined
symbol that cannot be resolved at run time.

Why: checking only the 345 UND symbols of camera.vendor.tegra.so is not enough -
its dependencies (libcuda.so / libphs.so / libnvmm_camera_v3.so ...) can also
reference symbols this ROM does not have, which breaks the whole dlopen chain.
(Seen for real: libcuda.so -> NvPHSSetThrottle, while libphs.so only exports
NvPHSSendThroughputHints.)

Usage:
    python3 depcheck.py <root.so> [--lib-dirs dir1 dir2 ...]
"""

import argparse
import os
import subprocess
import sys
from collections import deque


def readelf_dyn(path, args):
    try:
        out = subprocess.run(["readelf", "--dyn-syms", "-W", path],
                             capture_output=True, text=True, errors="replace")
    except Exception:
        return []
    if out.returncode != 0:
        return []
    rows = []
    for line in out.stdout.splitlines():
        # layout:  194: 00000000     0 FUNC    GLOBAL DEFAULT  UND _ZN...
        parts = line.split()
        if len(parts) < 8 or not parts[0].endswith(":"):
            continue
        rows.append(parts)
    return rows


def get_exported(path):
    """Return the set of symbol names this library exports (UND excluded)."""
    syms = set()
    for p in readelf_dyn(path, "--dyn-syms -W"):
        # p: [num, value, size, type, bind, vis, ndx, name]
        if len(p) < 8:
            continue
        ndx, typ, name = p[6], p[3], p[7]
        if ndx == "UND":
            continue
        if typ in ("FUNC", "OBJECT", "NOTYPE", "IFUNC", "GNU_IFUNC"):
            syms.add(name.split("@")[0])
    return syms


def get_needed(path):
    out = subprocess.run(["readelf", "-d", "-W", path],
                         capture_output=True, text=True, errors="replace")
    needed = []
    for line in out.stdout.splitlines():
        if "(NEEDED)" in line:
            m = line.split("[")
            if len(m) == 2:
                needed.append(m[1].rstrip("]"))
    return needed


def get_undefined(path):
    und = set()
    for p in readelf_dyn(path, "--dyn-syms -W"):
        if len(p) < 8:
            continue
        ndx, name = p[6], p[7]
        if ndx == "UND" and name:
            und.add(name.split("@")[0])
    return und


def find_lib(name, lib_dirs):
    for d in lib_dirs:
        cand = os.path.join(d, name)
        if os.path.exists(cand):
            return cand
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--lib-dirs", nargs="+", required=True)
    ap.add_argument("--recurse-only-in", nargs="*", default=None,
                    help="only recurse into libraries under these directories; "
                         "everything else is treated as a symbol provider only")
    ap.add_argument("--extra-symbols", default=None,
                    help="extra symbol pool file, one symbol per line. Usually the "
                         "union of exports from all system libraries, used to recognise "
                         "symbols provided by framework libraries")
    args = ap.parse_args()

    lib_dirs = args.lib_dirs
    recurse_only_in = [os.path.abspath(d) for d in (args.recurse_only_in or [])]

    def should_recurse(path):
        if not recurse_only_in:
            return True
        return any(path.startswith(d + os.sep) for d in recurse_only_in)

    # 1. walk the dependency tree breadth first
    tree = {}          # realpath -> list of needed SONAMEs
    missing_files = []
    queue = deque([os.path.abspath(args.root)])
    visited = set()
    while queue:
        cur = queue.popleft()
        if cur in visited:
            continue
        visited.add(cur)
        needed = get_needed(cur)
        tree[cur] = needed
        # System framework libraries (the ones under system/lib) are only used as a
        # symbol source and are not traversed, otherwise the walk spreads through
        # libstagefright/libcodec2 into the whole media stack.
        if not should_recurse(cur):
            continue
        for n in needed:
            p = find_lib(n, lib_dirs)
            if p is None:
                missing_files.append((cur, n))
            elif p not in visited:
                queue.append(p)

    print(f"dependency tree: {len(tree)} libraries\n")

    # 2. collect the symbols exported by every library in the tree (resolvable
    #    within one namespace)
    exported = set()
    for p in tree:
        exported |= get_exported(p)

    if args.extra_symbols and os.path.exists(args.extra_symbols):
        with open(args.extra_symbols, "r", errors="replace") as f:
            for line in f:
                s = line.strip()
                if s:
                    exported.add(s.split("@")[0])
        print(f"(extra symbols loaded, pool now {len(exported)})")

    # 3. per library, find the UND symbols that cannot be resolved
    print("=" * 78)
    print("unresolved symbols, grouped by library")
    print("=" * 78)
    total = 0
    # These are stable ABI from libc/libm/libdl and friends; readelf cannot read
    # their symbols, which is expected.
    ignore_prefix = ("__aeabi_", "__gnu_", "_Unwind_")

    for p in sorted(tree):
        und = get_undefined(p)
        miss = sorted(s for s in und if s not in exported and not s.startswith(ignore_prefix))
        if miss:
            total += len(miss)
            print(f"\n### {p}")
            for s in miss:
                # also try demangling so a human can read it
                d = subprocess.run(["c++filt", s], capture_output=True, text=True).stdout.strip()
                print(f"    {s}" + (f"\n        -> {d}" if d != s else ""))

    print("\n" + "=" * 78)
    if missing_files:
        print("missing library files:")
        for src, n in missing_files:
            print(f"    {n}   (needed by {os.path.basename(src)})")
    else:
        print("missing library files: none")

    print(f"\nunresolved symbols in total: {total}")
    return 0 if total == 0 and not missing_files else 1


if __name__ == "__main__":
    sys.exit(main())
