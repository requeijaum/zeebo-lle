#!/usr/bin/env python3
"""Meta-gate: no mutation guard may report a SKIP (exit 77) as a reproduced RED.

Several commercial/asset-dependent gates run a "buggy" mutant and assert it
FAILS as expected. The historical form was:

    @if ./test_foo buggy >/dev/null 2>&1; then \
      echo "FAIL: mutante passou"; exit 1; \
    else echo "OK: falha como esperado (RED reproduzido)"; fi

When the required proprietary asset is ABSENT the mutant does not fail on its
merits -- it SKIPs with exit 77. The bare `else` branch then mislabels that
SKIP as "RED reproduzido": a FALSE GREEN. A skipped mutant proves nothing.

Rule enforced here (static): every mutation guard whose target binary can exit
77 (its primary run line tolerates `[ $? -eq 77 ]`, i.e. the gate is
asset/NAND dependent) MUST branch on exit code 77 explicitly inside the guard
so a skipped mutant is reported as SKIP, never as a reproduced RED.

Negative-mutation property: revert any fixed guard back to the bare
if/then/else form and this gate turns RED (exit 1).

No NAND required. Pure static scan of tools/cpp/Makefile.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MK = os.path.join(HERE, "Makefile")

# A mutant-guard invokes a test binary with a mutation argument, e.g.
#   @if ./test_foo buggy >/dev/null 2>&1; then \        (old bare form)
#   @./test_foo buggy >/dev/null 2>&1; rc=$$?; \        (fixed 77-aware form)
# The mutation argument is a bare word (buggy, derived, sync, mutant-subbank,
# 4, ...) -- never a shell operator (||, >, [) and never a make flag.
MUTANT_INVOKE_RE = re.compile(r"\./(test_[A-Za-z0-9_]+)\s+([A-Za-z0-9][\w-]*)")
# The 77-tolerant primary run line: ./test_foo ... || [ $$? -eq 77 ]
RUN77_RE = re.compile(r"\./(test_[A-Za-z0-9_]+)\b.*\[\s*\$\$\?\s*-eq\s*77\s*\]")
# Alternate primary-run form used by semantic/provenance gates:
#   ./test_foo; rc=$$?; \
#   ... elif [ $$rc -eq 77 ] ...
RUN_RC77_RE = re.compile(
    r"\./(test_[A-Za-z0-9_]+)\s*;\s*rc=\$\$\?.*?-eq\s*77",
    re.DOTALL,
)


def parse_recipe_blocks(lines):
    """Yield (target, [recipe_lines]) blocks."""
    rule_re = re.compile(r"^([A-Za-z0-9_.+-]+)\s*:(?!=)")
    cur = None
    recipe = []
    for line in lines:
        if line.startswith("\t"):
            recipe.append(line)
            continue
        if cur is not None:
            yield cur, recipe
        m = rule_re.match(line)
        cur = m.group(1) if m else None
        recipe = []
    if cur is not None:
        yield cur, recipe


def main():
    with open(MK, encoding="utf-8") as f:
        lines = f.read().splitlines()

    # Which binaries can legitimately exit 77 (asset/NAND dependent)?
    can_skip77 = set()
    for ln in lines:
        m = RUN77_RE.search(ln)
        if m:
            can_skip77.add(m.group(1))
    make_text = "\n".join(lines)
    for m in RUN_RC77_RE.finditer(make_text):
        can_skip77.add(m.group(1))

    problems = []
    checked = 0
    for target, recipe in parse_recipe_blocks(lines):
        i = 0
        n = len(recipe)
        while i < n:
            m = MUTANT_INVOKE_RE.search(recipe[i])
            if not m:
                i += 1
                continue
            binary = m.group(1)
            # gather the guard block (this line + its line-continuations)
            guard = [recipe[i]]
            j = i
            while recipe[j].rstrip().endswith("\\") and j + 1 < n:
                j += 1
                guard.append(recipe[j])
            guard_text = "\n".join(guard)
            i = j + 1
            if binary not in can_skip77:
                continue  # host-only mutant: never exits 77, bare form is fine
            checked += 1
            # A 77-capable mutant guard MUST branch on exit code 77 so a skipped
            # mutant is reported as SKIP, never as a reproduced RED.
            if not re.search(r"-eq\s*77", guard_text):
                problems.append(
                    f"{target}: mutant guard on '{binary}' (77-capable) does not "
                    f"branch on exit 77 -> a SKIP (77) is falsely reported as "
                    f"'RED reproduced' (false green)."
                )

    print(f"[mutation-gate-hygiene] {checked} 77-capable mutant guard(s) checked; "
          f"77-tolerant binaries: {', '.join(sorted(can_skip77)) or '(none)'}")
    if problems:
        for p in problems:
            print(f"[mutation-gate-hygiene] FAIL: {p}")
        print(f"\nmutation-gate-hygiene GATE RED: {len(problems)} false-green guard(s).")
        return 1
    print("mutation-gate-hygiene GATE GREEN: no SKIP-77 is reported as a reproduced RED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
