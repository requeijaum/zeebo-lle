"""Unicorn semantics witness, NOT actual production-handler regression."""
import json
import struct
from pathlib import Path
import unicorn
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_R0

cases = []
for extra in (False, True):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(0x1000, 0x1000)
    uc.mem_write(0x1000, struct.pack('<III', 0xef000014, 0xe3a0005a, 0xe1a00000))
    events = []
    def intr(engine, number, data):
        pc = engine.reg_read(UC_ARM_REG_PC)
        events.append({'interrupt': number, 'pc': hex(pc)})
        if extra:
            engine.reg_write(UC_ARM_REG_PC, pc + 4)
    uc.hook_add(UC_HOOK_INTR, intr)
    uc.emu_start(0x1000, 0x100c)
    cases.append({'extra_pc_plus_4': extra, 'events': events,
                  'r0': hex(uc.reg_read(UC_ARM_REG_R0))})
assert cases[0]['events'][0]['pc'] == '0x1004'
assert cases[0]['r0'] == '0x5a'
assert cases[1]['r0'] == '0x0'
report = {'unicorn': unicorn.__version__, 'scope': 'semantics witness; not integrated boot', 'cases': cases}
Path(__file__).with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
