#!/usr/bin/env python3
"""QW20 — Load-bearing clean-hygiene gate.

Static check: every test/binary produced by a LINK target (a rule whose recipe
compiles/links with `-o <name>`) in tools/cpp/Makefile and gpu/Makefile.gpu MUST
be listed in the `clean` target of the SAME Makefile.

This runs INSIDE `make check` WITHOUT destroying build artifacts (it never
executes `make clean`), so it cannot delete binaries that later check targets
depend on.

Negative-mutation property: removing one binary name from a clean list turns
this gate RED (exit 1). See ROADMAP QW20.

No NAND required. Does not touch qdsp5/.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT_MK = os.path.join(HERE, "Makefile")
GPU_MK = os.path.join(HERE, "gpu", "Makefile.gpu")

RULE_RE = re.compile(r"^([A-Za-z0-9_.+-]+)\s*:(?!=)")


def read_lines(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read().splitlines()


def parse_rules(lines):
    """Return dict: target_name -> list of recipe lines (tab-prefixed)."""
    rules = {}
    current = None
    for line in lines:
        if line.startswith("\t"):
            if current is not None:
                rules.setdefault(current, []).append(line)
            continue
        m = RULE_RE.match(line)
        if m:
            current = m.group(1)
            rules.setdefault(current, [])
        else:
            # non-recipe, non-rule line (blank/var/comment) closes recipe scope
            if line.strip() == "" or not line.startswith("\t"):
                current = None
    return rules


def link_targets(rules):
    """Targets whose recipe links an output binary named after the target.

    A target `foo` is a link target when any recipe line contains `-o $@`
    or `-o foo` (explicit name). Phony/aggregate targets never match.
    """
    out = []
    for name, recipe in rules.items():
        if name.startswith("."):
            continue
        for rl in recipe:
            if re.search(r"-o\s+\$@(\s|$)", rl) or re.search(
                r"-o\s+" + re.escape(name) + r"(\s|$)", rl
            ):
                out.append(name)
                break
    return sorted(set(out))


def clean_tokens(lines):
    """Collect all whitespace-separated tokens in the `clean` target recipe,
    joining backslash line-continuations."""
    rules = parse_rules(lines)
    recipe = rules.get("clean", [])
    # join continuations
    joined = []
    buf = ""
    for rl in recipe:
        body = rl.rstrip("\n")
        if body.rstrip().endswith("\\"):
            buf += body.rstrip()[:-1] + " "
        else:
            buf += body
            joined.append(buf)
            buf = ""
    if buf:
        joined.append(buf)
    tokens = set()
    for j in joined:
        for tok in j.split():
            tokens.add(tok)
    return tokens


def check_makefile(path, label):
    lines = read_lines(path)
    rules = parse_rules(lines)
    targets = link_targets(rules)
    covered = clean_tokens(lines)
    missing = [t for t in targets if t not in covered]
    print(f"[{label}] {len(targets)} link target(s): {', '.join(targets)}")
    if missing:
        for m in missing:
            print(f"[{label}] FAIL: binary '{m}' is produced but NOT in `clean`")
    else:
        print(f"[{label}] OK: all link-target binaries covered by `clean`")
    return missing


def check_no_tracked_ignored():
    """Gate: no file that git considers ignored may still be tracked in the index.

    A tracked-yet-ignored file (e.g. a committed build binary or *.pyc) means the
    index is polluted: a clean clone ships artifacts the .gitignore claims to
    exclude, and `git status` stays silent about drift in them. We ask git itself
    for the authoritative intersection so this never hard-codes a path list.

    Negative-mutation property: `git add -f` any ignored file and re-run — this
    gate turns RED (exit 1). Removing it from the index (`git rm --cached`) turns
    it GREEN again.
    """
    repo_root = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        cwd=HERE, capture_output=True, text=True,
    )
    if repo_root.returncode != 0:
        print("[tracked-ignored] SKIP: not a git work tree")
        return []
    root = repo_root.stdout.strip()
    # -c: ignored files that ARE tracked (in the index). Authoritative.
    res = subprocess.run(
        ["git", "ls-files", "-z", "-i", "-c", "--exclude-standard"],
        cwd=root, capture_output=True, text=True,
    )
    if res.returncode != 0:
        print(f"[tracked-ignored] SKIP: git ls-files failed: {res.stderr.strip()}")
        return []
    tracked_ignored = [p for p in res.stdout.split("\0") if p]
    if tracked_ignored:
        for p in tracked_ignored:
            print(f"[tracked-ignored] FAIL: ignored file is tracked in index: {p}")
    else:
        print("[tracked-ignored] OK: no ignored file is tracked in the index")
    return tracked_ignored


def main():
    fails = []
    fails += check_makefile(ROOT_MK, "root")
    fails += check_makefile(GPU_MK, "gpu")
    tracked_ignored = check_no_tracked_ignored()
    fails += tracked_ignored
    if fails:
        print(
            f"\nclean-hygiene GATE RED: {len(fails)} problem(s) "
            f"({len(tracked_ignored)} tracked-ignored file(s))."
        )
        return 1
    print("\nclean-hygiene GATE GREEN: workspace stays 100% pure post-clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
