# Path C validity correction

The original `/tmp/zeebo-boot-replay/notes/boot-investigation/path-c/REPORT.md` does not describe the intended APPS baseline. Do not use its NOP-slide/crash as evidence that APPS cannot reach Iguana.

Parent verification read both working ELF files through their PT_LOAD mappings:

| VA | APPS first 16 bytes | AMSS first 16 bytes |
|---|---|---|
| b000c738 | 140000ef000054e300108415000055e3 | 03f0a0e10e30a0e10dc0a0e127d0e0e3 |
| b000d494 | a1fcffebc83090e5ff2fc3e30320c2e3 | 012052e2000483e5080083e5043083e2 |
| b000d6dc | 004084e01833a011070054e1035085e0 | a220b0e1013083e2fcffff1a0320a0e1 |

Original captured bytes match AMSS at every listed address. Reported Core0 PC near 00a00000 is consistent with AMSS ELF entry; APPS ELF entry is 10000000. Hashing the intended firmware files alone did not prove which one the CLI loaded.

Source cause: zeebo_lle_main.cpp:2993-2998 uses equality to default filename strings as slot-occupancy sentinels. Explicitly supplying the default NAND path leaves the first condition true, so the next filename overwrites NAND; the third becomes APPS. Retry with three absolute paths and validate logged selections plus loaded bytes before any execution claim.

Additional capture script defect: existing ZeeboDebugClient.reg returns an integer, read_mem returns bytes. The original script calls .get on both, catches exceptions and records errors instead of evidence.

The observation of static repeatability is not a pre-stall replay verification. Original artifacts remain preserved; corrected capture and separate TDD CLI fix have been dispatched.
