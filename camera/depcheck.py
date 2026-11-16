#!/usr/bin/env python3
"""
递归分析一个 ELF 库的整条依赖树，找出所有运行时无法解析的未定义符号。

背景：只检查 camera.vendor.tegra.so 自己的 345 个 UND 符号是不够的 ——
它的依赖（libcuda.so / libphs.so / libnvmm_camera_v3.so ...）自身也可能引用
在本 ROM 上不存在的符号，导致整条 dlopen 链断掉。
（实际踩到：libcuda.so -> NvPHSSetThrottle，libphs.so 里只有 NvPHSSendThroughputHints。）

用法：
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
        # 形如：  194: 00000000     0 FUNC    GLOBAL DEFAULT  UND _ZN...
        parts = line.split()
        if len(parts) < 8 or not parts[0].endswith(":"):
            continue
        rows.append(parts)
    return rows


def get_exported(path):
    """返回该库导出的符号名集合（排除 UND）。"""
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

    # 1. BFS 遍历依赖树
    tree = {}          # realpath -> 依赖的 SONAME 列表
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
        # 系统 framework 库（system/lib 下那些）只用来提供符号，不深入遍历，
        # 否则会顺着 libstagefright/libcodec2 一路扩散到整个媒体栈。
        if not should_recurse(cur):
            continue
        for n in needed:
            p = find_lib(n, lib_dirs)
            if p is None:
                missing_files.append((cur, n))
            elif p not in visited:
                queue.append(p)

    print(f"依赖树共 {len(tree)} 个库\n")

    # 2. 收集全树中所有库导出的符号（同一个 namespace 下可互相解析）
    exported = set()
    for p in tree:
        exported |= get_exported(p)

    if args.extra_symbols and os.path.exists(args.extra_symbols):
        with open(args.extra_symbols, "r", errors="replace") as f:
            for line in f:
                s = line.strip()
                if s:
                    exported.add(s.split("@")[0])
        print(f"（额外载入符号池：{len(exported)} 个）")

    # 3. 逐个库找无法解析的 UND
    print("=" * 78)
    print("无法解析的符号（按库分组）")
    print("=" * 78)
    total = 0
    # 这些是 libc/libm/libdl 等稳定 ABI，readelf 从这些库读不到符号属正常
    ignore_prefix = ("__aeabi_", "__gnu_", "_Unwind_")

    for p in sorted(tree):
        und = get_undefined(p)
        miss = sorted(s for s in und if s not in exported and not s.startswith(ignore_prefix))
        if miss:
            total += len(miss)
            print(f"\n### {p}")
            for s in miss:
                # 顺便尝试 demangle，便于人工判断
                d = subprocess.run(["c++filt", s], capture_output=True, text=True).stdout.strip()
                print(f"    {s}" + (f"\n        -> {d}" if d != s else ""))

    print("\n" + "=" * 78)
    if missing_files:
        print("缺失的库文件：")
        for src, n in missing_files:
            print(f"    {n}   (被 {os.path.basename(src)} 需要)")
    else:
        print("缺失的库文件：无")

    print(f"\n合计无法解析符号：{total}")
    return 0 if total == 0 and not missing_files else 1


if __name__ == "__main__":
    sys.exit(main())
