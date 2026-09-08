# CLI positional firmware-path assignment fix (path-cli)

## Defect (definitive, byte-level)
`tools/cpp/zeebo_lle_main.cpp` main() (previously lines ~2993-2998) tracked
positional-slot occupancy by comparing the current path value against the
default string:

```cpp
if (std::string(nand_path) == "../../nand/1.1.2.bin") nand_path = argv[i];
else if (std::string(apps_path) == "../../nand/1.1.2_APPS.bin") apps_path = argv[i];
else if (std::string(amss_path) == "../../nand/1.1.2_AMSS.bin") amss_path = argv[i];
```

Supplying the three positionals
`../../nand/1.1.2.bin ../../nand/1.1.2_APPS.bin ../../nand/1.1.2_AMSS.bin`
(first token equals the default NAND path) makes slot 0 still "look empty",
so:
- NAND slot is overwritten with the **APPS** path,
- APPS slot is then overwritten with the **AMSS** path,
- Core0 loads **AMSS bytes as APPS** (confirmed by parent comparing the PathC
  memory region with the ELF bytes).

## Fix
Extracted the slot assignment into pure, host-testable
`resolve_cli_firmware_paths()` (`tools/cpp/zeebo_cli_paths.h`) that assigns by
ordinal INDEX (0=NAND, 1=APPS, 2=AMSS) and explicitly flags surplus (4th+)
positionals. main() now collects positionals then calls the resolver.
No broad refactor; no firmware-dependent CI test.

## Strict TDD trail
- The extracted resolver first carried the *verbatim* buggy string-equality
  logic; the test exercised that real production logic and went RED.
- RED (against unmodified buggy logic): 3 failing assertions —
  `explicit-default.nand` got APPS path, `explicit-default.apps` got AMSS path,
  `surplus.flag` not raised.
- GREEN: index-based assignment; all 17 assertions pass.

## Cases covered
omitted defaults / explicit default strings / absolute paths / NAND-only /
surplus args. (No mixed-options case at resolver level since option parsing is
orthogonal and unchanged; the invalid-arg exit-code checks in `test-cli`
remain.)

## Commands
RED  (revert header to string-equality form, then):
    make -C tools/cpp -B test_cli_paths && ./tools/cpp/test_cli_paths   # exit 1
GREEN:
    make -C tools/cpp -B test_cli_paths && ./tools/cpp/test_cli_paths   # exit 0
Aggregate CLI target:
    make -C tools/cpp test-cli
Production still builds:
    make -C tools/cpp zeebo_lle_main

## Files
- tools/cpp/zeebo_cli_paths.h      (new: resolver)
- tools/cpp/test_cli_paths.cpp     (new: RED/GREEN regression harness)
- tools/cpp/zeebo_lle_main.cpp     (wire main() to resolver)
- tools/cpp/Makefile               (test_cli_paths target + clean + check)
