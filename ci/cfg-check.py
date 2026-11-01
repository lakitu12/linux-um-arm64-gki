#!/usr/bin/env python3
"""Strict per-symbol audit of the kernel fragments this repo merges.

usage: cfg-check.py <build-.config> <fragment> [fragment...]

For every `CONFIG_X=v` / `# CONFIG_X is not set` line a fragment asks for,
check the final .config:

  ok       exact expected state present
  DROPPED  the symbol is absent from the .config entirely — olddefconfig
           removed it (name typo, unmet dependency, not in this tree).
           These are the silent ones: no "# ... is not set" marker, the
           request just vanishes.  This is the class the audit exists for.
  WRONG    the symbol is present with a different value (dependencies
           forced it, or a later fragment overrode an earlier one).

Exit 0 only when every request in every fragment is satisfied; any
DROPPED/WRONG fails the run with the symbol named on stdout.
"""
import re
import sys

if len(sys.argv) < 3:
    print("usage: cfg-check.py <build-.config> <fragment> [fragment...]")
    sys.exit(2)

final = open(sys.argv[1], encoding="utf-8", errors="replace").read()
state = {}
for m in re.finditer(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", final, re.M):
    state[m.group(1)] = m.group(2)
for m in re.finditer(r"^# (CONFIG_[A-Z0-9_]+) is not set$", final, re.M):
    state[m.group(1)] = None

bad = 0
for frag in sys.argv[2:]:
    ok = dropped = wrong = 0
    print(f"== {frag} ==")
    for line in open(frag, encoding="utf-8"):
        line = line.strip()
        m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line)
        if m:
            k, v = m.group(1), m.group(2)
            got = state.get(k, "<absent>")
            if got == v:
                ok += 1
            elif got == "<absent>":
                dropped += 1
                print(f"  DROPPED  {k}: wanted {v!r}, symbol absent from .config")
            else:
                wrong += 1
                print(f"  WRONG    {k}: wanted {v!r}, got {got!r}")
            continue
        m = re.match(r"^# (CONFIG_[A-Z0-9_]+) is not set$", line)
        if m:
            k = m.group(1)
            got = state.get(k, "<absent>")
            if got is None:
                ok += 1
            elif got == "<absent>":
                dropped += 1
                print(f"  DROPPED  # {k} is not set (symbol absent)")
            else:
                wrong += 1
                print(f"  WRONG    {k}: wanted not-set, got {got!r}")
    print(f"  -> ok={ok} dropped={dropped} wrong={wrong}")
    bad += dropped + wrong

print("cfg-check: ALL-GREEN" if bad == 0 else f"cfg-check: ISSUES: {bad}")
sys.exit(1 if bad else 0)