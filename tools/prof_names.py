#!/usr/bin/env python3
"""prof_names.py <debug.log> [--all]: the [prof] (and watchdog) lines of a hardware log with their addresses named.

  GameAssembly+X   the Mac game's IL2CPP code: ../analysis/ga_syms.txt (symbol = X - 0x40000: Project Dream's
                   atomics stubs come first in its code memory)
  libunity.so+X    ../analysis/libunity_syms.txt (Unity's symbol file for this libunity.so)
  libil2cpp.so+X   Project Dream's loader, by its own symbol table (nm)
  nro+X            projectdream_nx.elf (addr2line)

Without --all only [prof] lines are printed.
"""
import bisect
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
A64 = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-"


def table_from_lines(lines, addr_col, name_col):
    rows = []
    for l in lines:
        p = l.split()
        if len(p) > max(addr_col, name_col):
            try:
                rows.append((int(p[addr_col], 16), p[name_col]))
            except ValueError:
                pass
    rows.sort()
    return [r[0] for r in rows], rows


def load(path, addr_col, name_col):
    if not os.path.exists(path):
        return [], []
    with open(path, errors="replace") as f:
        return table_from_lines(f, addr_col, name_col)


GA = load(os.path.join(ROOT, "analysis", "ga_syms.txt"), 0, 1)
LU = load(os.path.join(ROOT, "analysis", "libunity_syms.txt"), 0, 2)
loader = os.path.join(ROOT, "analysis", "decomp", "proj", "Android.prebuilt.libil2cpp.so")
PD = table_from_lines(subprocess.run([A64 + "nm", "-n", loader], capture_output=True, text=True).stdout.splitlines(),
                      0, 2) if os.path.exists(loader) else ([], [])
ELF = os.path.join(HERE, "..", "projectdream_nx.elf")
_dem = {}


def demangle(n):
    if n not in _dem:
        m = n[1:] if n.startswith("__Z") else n
        out = subprocess.run(["c++filt", "-n", m], capture_output=True, text=True).stdout.strip() or n
        out = re.sub(r"_m[0-9A-F]{30,}", "", out)
        _dem[n] = out if len(out) < 120 else out[:117] + "..."
    return _dem[n]


def lookup(tab, a):
    addrs, rows = tab
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return None
    s, n = rows[i]
    return "%s+0x%x" % (demangle(n), a - s)


_nro = {}


def nro_name(a):
    if a not in _nro:
        out = subprocess.run([A64 + "addr2line", "-f", "-C", "-e", ELF, hex(a)], capture_output=True,
                             text=True).stdout.split("\n")
        _nro[a] = out[0] if out and out[0] != "??" else None
    return _nro[a]


def name(mod, off):
    a = int(off, 16)
    if mod == "GameAssembly":
        return lookup(GA, a - 0x40000)
    if mod == "libunity.so":
        return lookup(LU, a)
    if mod == "libil2cpp.so":
        return lookup(PD, a)
    if mod == "nro":
        return nro_name(a)
    return None


ADDR = re.compile(r"\b(GameAssembly|libunity\.so|libil2cpp\.so|nro)\+(?:0x)?([0-9a-f]+)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    every = "--all" in sys.argv
    for line in open(sys.argv[1], errors="replace"):
        if not every and "[prof]" not in line:
            continue
        def sub(m):
            n = name(m.group(1), m.group(2))
            return "%s {%s}" % (m.group(0), n) if n else m.group(0)
        sys.stdout.write(ADDR.sub(sub, line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
