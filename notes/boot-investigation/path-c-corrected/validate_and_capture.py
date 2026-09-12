#!/usr/bin/env python3
# Path C CORRECTED: validate APPS bytes loaded (not AMSS), then forensic capture.
# Fixes: reg() returns int, read_mem() returns bytes (NOT .get()).
import sys, os, json, time
sys.path.insert(0, "/tmp/zeebo-boot-replay/tools/cpp")
from zeebo_debug_scripting import ZeeboDebugClient

PORT = int(os.environ.get("ZEEBO_PORT", "49103"))
OUT = os.path.dirname(os.path.abspath(__file__))

# Expected APPS signatures (parent-provided). AMSS = the WRONG bytes seen before.
EXPECT = {
    0xb000c738: ("APPS", "140000ef000054e3", "AMSS", "03f0a0e10e30a0e1"),
    0xb000d494: ("APPS", "a1fcffebc83090e5", "AMSS", "012052e2000483e5"),
    0xb000d6dc: ("APPS", "004084e01833a011", "AMSS", "a220b0e1013083e2"),
}

def hx(b): return b.hex()

def main():
    c = ZeeboDebugClient(port=PORT)
    c.connect(timeout=15)
    st0 = c.state()
    result = {"port": PORT, "initial_state": st0, "byte_validation": {}, "verdict": None}
    print("[probe] connected; state:", st0)

    all_apps = True
    any_amss = False
    for addr, (an, ahex, mn, mhex) in EXPECT.items():
        b = c.read_mem(0, addr, 16)
        got = hx(b)
        is_apps = got.startswith(ahex.lower())
        is_amss = got.startswith(mhex.lower())
        all_apps = all_apps and is_apps
        any_amss = any_amss or is_amss
        result["byte_validation"][hex(addr)] = {
            "got16": got, "expect_apps": ahex, "expect_amss": mhex,
            "matches_apps": is_apps, "matches_amss": is_amss,
        }
        print(f"[probe] {hex(addr)}: got={got}  APPS={is_apps} AMSS={is_amss}")

    result["verdict"] = "APPS_CORRECT" if all_apps else ("STILL_AMSS" if any_amss else "UNKNOWN")
    print("[probe] verdict:", result["verdict"])

    with open(os.path.join(OUT, "byte_validation.json"), "w") as f:
        json.dump(result, f, indent=2, default=str)
    try: c.quit()
    except Exception: pass
    return result["verdict"]

if __name__ == "__main__":
    v = main()
    sys.exit(0 if v == "APPS_CORRECT" else 3)
