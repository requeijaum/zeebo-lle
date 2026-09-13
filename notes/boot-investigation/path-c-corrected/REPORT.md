# Path C — CORRECTED Forensic Capture (APPS/AMSS CLI misassignment fixed)

Worktree: `/tmp/zeebo-boot-replay` @ `ea1b48f`
Binary: `tools/cpp/zeebo_lle_main` sha256 `f54102d9fa9b1057eda1a4ba73cece9d16705f732d0fcaa9555306005fd33c8a`
Output dir: `/home/rafaelfrequiao/projects/zeebo-lle/notes/boot-investigation/path-c-corrected/`

## Root cause of the original Path C invalidity — CONFIRMED
`main.cpp` positional-arg parsing (lines ~2992-2998) uses *equality-to-default* as an
"unset" sentinel:
```cpp
if (nand_path == nullptr || std::string(nand_path) == "../../nand/1.1.2.bin") { nand_path = argv[i]; }
else if (apps_path == nullptr || std::string(apps_path) == "../../nand/1.1.2_APPS.bin") { apps_path = argv[i]; }
else if (amss_path == nullptr || std::string(amss_path) == "../../nand/1.1.2_AMSS.bin") { amss_path = argv[i]; }
```
Passing the default nand path (`../../nand/1.1.2.bin`) **explicitly** left the nand slot
still matching its sentinel → the *next* positional (`..._APPS.bin`) overwrote `nand_path`,
shifting every subsequent file down one slot. APPS therefore received the AMSS image
(entry `0x00a00000`), which is exactly why the parent byte-verified
`c738=03f0a0e1…, d494=012052e2…, d6dc=a220b0e1…` = **AMSS PT_LOAD**, not APPS.

## Fix applied (invocation only — NO C++/production change)
Launch with **all three ABSOLUTE paths** so no argument equals a default sentinel:
```
./tools/cpp/zeebo_lle_main --control-port=49103 --headless --seconds=600 \
  /home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2.bin \
  /home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPS.bin \
  /home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_AMSS.bin
```
Working copies only (worktree `nand/*.bin` are symlinks to the main-repo read-only dump).

## Loaded-byte validation — PASS (`byte_validation.json`)
Live `read_mem(core0)` after correct load; expected APPS bytes match, AMSS bytes absent:

| addr | got (16B) | APPS? | AMSS? |
|------|-----------|-------|-------|
| `0xb000c738` | `140000ef000054e300108415000055e3` | ✅ | ✗ |
| `0xb000d494` | `a1fcffebc83090e5ff2fc3e30320c2e3` | ✅ | ✗ |
| `0xb000d6dc` | `004084e01833a011070054e1035085e0` | ✅ | ✗ |

Verdict: **APPS_CORRECT**. The exact bytes the parent said APPS *should* have are now present.

## Genuine pre-KIP / stall evidence — CAPTURED (`forensic_capture.json`)
Read-only debugger (`ZeeboDebugClient`, port 49103), cold boot, bp-driven — no register forcing.
This is now **real execution**, not the old NOP-slide: Core0 executes ~1.47M genuine Iguana
instructions and issues thousands of real `L4_MapControl` syscalls before looping.

Disassembly (capstone ARM) of the captured code windows:
- `d494`: `bl #0xb000c720` (L4_KernelInterface) → `d498`: `ldr r3,[r0,#0xc8]` = **`l4e_min_pagesize`**
  reading `KIP+0xc8` (PageInfo). This is the documented minpage/CTZ path, NOT a mailbox poll.
- `d6dc`: `add r4,r4,r0 / lslne r3,r8,r3 / cmp r4,r7` — the documented pool-advance loop.
- `d708`: `lsr r3,lr,#4 / ands r3,r3,#0x3f / lslne r3,r8,r3` — fpage `size_log2` extraction.

### Captured runtime state (the frames the original report could NOT obtain)
**minpage caller LR `d498`** (c0_insns=118428):
- pc=lr=`0xb000d498`, r0=`0xf0f00000` (KIP base), sp=`0xb0041d0c`
- KIP `0xf0f000c8` = `0610110100000000` → PageInfo `0x01111006` (seeded, ARMv6 4K/64K/1M/16M)
- `b0041284` (cached minpage) = still `0` at this point (minpage not yet stored)
- backtrace frames: `d498 → d4d0 → d504 → 0x10000000(APPS ELF base) → d5b4 → d3a8 → …`

**stall `d6dc`** (c0_insns=118959): r4=`0xb0d00000` (virt pool cursor), r5=`0x10000000`,
r6=`0x300`, r7=`0xb6d00000` (virt_end). sp=`0xb0041d14`. Pool-decomposition loop, matches
the skill's documented `mempool_init` fpage walk.

**stall `d708`** (c0_insns=120029, and still looping at 1.47M insns, `running:true`):
- pc=`0xb000d708`, lr=`0x380` (=896), r0=`0x100`, r1=r2=`0x10000` (64K), r8=`1`
- `size_log2 = (lr>>4)&0x3f = (0x380>>4)&0x3f = 0x38 = 56` → `lslne r8(1),56` = **0** on 32-bit ARM
  → zero advance → **infinite loop** (same LSL-by-≥32 family the skill documents, now reached
  in *genuine execution* rather than the AMSS-misload slide).

## KEY DIFFERENCE vs. the (invalid) original Path C
| | ORIGINAL (invalid, AMSS-in-APPS) | CORRECTED (this run) |
|--|--|--|
| APPS bytes | AMSS PT_LOAD | Correct APPS ✅ |
| Core0 behavior | NOP-slide `+0x9c40`/poll, never enters `0xb000xxxx` | Executes ~1.47M real Iguana insns |
| L4_MapControl | never called | thousands of real syscalls |
| Hooks fire | never | `d498`,`d6dc`,`d708` all hit with live regs/stack |
| Terminal state | phys_section assertion crash ~1M | genuine fpage-decomposition loop at `0xb000d708` |

## Scope / honesty
- Forensic dump of selected regs/stack/pages, NOT a full serialized snapshot; host device/audio
  callbacks are not frozen (`pause` freezes CPU stepping only).
- No C++ trace/production changes; QDSP5 untouched; one emulator instance max, port 49103.
- The `pre_kip_hook` at `0xb000c738` did not trap as a plain breakpoint (it is the hooked
  L4_KernelInterface tail / `svc` return site); its bytes are validated statically above and its
  caller path is visible in the `d498` backtrace.

## Artifacts (this dir)
- `validate_and_capture.py` — corrected byte validator (reg→int, read_mem→bytes; no `.get`)
- `forensic_capture.py` — cold-boot bp-driven live capture
- `byte_validation.json` — APPS byte-match proof
- `forensic_capture.json` — live regs/backtrace/stack/mem at d498, d6dc, d708
