#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 NXP
"""Static worst-case VCPU stack check for an iqplayer .eld.

Disassembles the ELD with vspa-ds, takes each function frame from its
'addS.z sp, +$N' prologue (N in 16-bit units, bytes = 2*N), builds the
call graph from 'jsr' targets and computes the deepest chain from every
root (function never called).  Fails (exit 1) when the deepest chain plus
the margin does not fit in ___stack_size read from the map file.

Overlays share run addresses (0x6000...): a call to such an address is
counted against every overlay function at that address (worst case).

Usage: stack_check.py <file.eld> <file.map> [--margin 64] [--verbose]
"""
import argparse
import os
import re
import subprocess
import sys

VSPA_TOOL = os.environ.get("VSPA_TOOL", "/opt/VSPA_Tools_vbeta_14_00_781")
VSPA_DS = os.path.join(VSPA_TOOL, "bin", "vspa-ds")

RE_TYPE = re.compile(r"^\s*\.type\s+(\S+),\s*@function")
RE_ADDR = re.compile(r"^#([0-9a-fA-F]+):")
RE_FRAME = re.compile(r"addS\.z\s+sp,\s*\+\$([0-9a-fA-F]+)")
RE_JSR = re.compile(r"\bjsr\s+(\S+?);(?:.*equivalent to \$([0-9a-fA-F]+)(?:\s*\((\S+)\))?)?")
RE_STACK = re.compile(r"0x([0-9a-fA-F]+)\s+___stack_size")


def parse_disasm(text):
    """Return {func: {'addr', 'frame', 'calls': [(name|None, addr|None)]}}."""
    funcs = {}
    cur = None
    want_addr = False
    for line in text.splitlines():
        m = RE_TYPE.match(line)
        if m:
            cur = m.group(1)
            funcs[cur] = {"addr": None, "frame": 0, "calls": [], "indirect": False}
            want_addr = True
            continue
        m = RE_ADDR.match(line)
        if m and cur and want_addr:
            # .type is followed by the first instruction; its address is
            # printed after it, so the function start is that address minus
            # the first bundle. Record the first address seen as an upper
            # bound and match calls by name when the name is available.
            funcs[cur]["addr"] = int(m.group(1), 16)
            want_addr = False
            continue
        if cur is None:
            continue
        m = RE_FRAME.search(line)
        if m:
            funcs[cur]["frame"] = max(funcs[cur]["frame"], int(m.group(1), 16) * 2)
        m = RE_JSR.search(line)
        if m:
            target = m.group(1)
            if not target.startswith("+$") and not target.startswith("$"):
                funcs[cur]["indirect"] = True
                continue
            addr = int(m.group(2), 16) if m.group(2) else None
            funcs[cur]["calls"].append((m.group(3), addr))
    return funcs


def resolve(funcs):
    """Map call sites to callee names."""
    by_name = set(funcs)
    by_addr = {}
    for name, f in funcs.items():
        if f["addr"] is not None:
            by_addr.setdefault(f["addr"], []).append(name)
    graph = {}
    unresolved = []
    for name, f in funcs.items():
        callees = set()
        for cname, caddr in f["calls"]:
            if cname and cname in by_name:
                callees.add(cname)
                continue
            # function start lines are printed one bundle after .type, so
            # accept a target within 8 bytes before a recorded address
            hits = []
            if caddr is not None:
                for d in range(0, 16, 1):
                    hits = by_addr.get(caddr + d, [])
                    if hits:
                        break
            if hits:
                callees.update(hits)
            else:
                unresolved.append((name, cname, caddr))
        graph[name] = sorted(callees)
    return graph, unresolved


def deepest(funcs, graph):
    memo = {}
    onstack = set()

    def walk(n):
        if n in memo:
            return memo[n]
        if n in onstack:
            raise RuntimeError("recursion through %s" % n)
        onstack.add(n)
        best, best_path = 0, []
        for c in graph.get(n, []):
            d, p = walk(c)
            if d > best:
                best, best_path = d, p
        onstack.discard(n)
        memo[n] = (funcs[n]["frame"] + best, [n] + best_path)
        return memo[n]

    called = {c for cs in graph.values() for c in cs}
    roots = [n for n in funcs if n not in called]
    worst = (0, [])
    for r in roots:
        d = walk(r)
        if d[0] > worst[0]:
            worst = d
    return worst, roots


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("eld")
    ap.add_argument("map")
    ap.add_argument("--margin", type=int, default=64)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    text = subprocess.run([VSPA_DS, a.eld], capture_output=True, text=True, check=True).stdout
    with open(a.map) as fh:
        maptext = fh.read()
    m = RE_STACK.search(maptext)
    if not m:
        print("stack_check: ___stack_size not found in %s" % a.map)
        return 2
    stack = int(m.group(1), 16)
    # The stack segment is declared in LCF memory space 2, so the linker
    # does not check it against .vdram bss: check the overlap here.
    mb = re.search(r"0x([0-9a-fA-F]+)\s+__bss_end\b", maptext)
    ms = re.search(r"0x([0-9a-fA-F]+)\s+__stack\b", maptext)
    if mb and ms and int(mb.group(1), 16) > int(ms.group(1), 16):
        print("stack_check: FAIL: __bss_end 0x%x overlaps the stack at __stack 0x%x"
              % (int(mb.group(1), 16), int(ms.group(1), 16)))
        return 1

    funcs = parse_disasm(text)
    graph, unresolved = resolve(funcs)
    indirect = [n for n, f in funcs.items() if f["indirect"]]
    try:
        (depth, path), roots = deepest(funcs, graph)
    except RuntimeError as e:
        print("stack_check: FAIL: %s" % e)
        return 1

    chain = " -> ".join("%s(%d)" % (n, funcs[n]["frame"]) for n in path)
    if a.verbose:
        for n in sorted(funcs, key=lambda k: -funcs[k]["frame"]):
            if funcs[n]["frame"]:
                print("  %5d B  %s -> %s" % (funcs[n]["frame"], n, ", ".join(graph[n])))
    for n, cname, caddr in unresolved:
        print("stack_check: warning: %s calls unknown %s @0x%x" % (n, cname, caddr or 0))
    if indirect:
        print("stack_check: FAIL: indirect calls in %s (add explicit handling)" % ", ".join(indirect))
        return 1

    print("stack_check: %s: worst %d B, stack %d B, margin %d B" % (os.path.basename(a.eld), depth, stack, a.margin))
    print("  %s" % chain)
    if depth + a.margin > stack:
        print("stack_check: FAIL: STACK OVERFLOW %d B + %d B margin > %d B" % (depth, a.margin, stack))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
