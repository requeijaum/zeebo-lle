#include <cassert>
#include <cstdint>
#include <unicorn/unicorn.h>
#include "zeebo_brew_loader.h"
#include "zeebo_zeetris_runner.h"

using zeebo::zeetris::ZeetrisRunner;

int main() {
    static_assert(ZeetrisRunner::key_event(true) == zeebo::brew::EVT_KEY_PRESS);
    static_assert(ZeetrisRunner::key_event(false) == zeebo::brew::EVT_KEY_RELEASE);

    uc_engine* uc = nullptr;
    assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
    assert(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL) == UC_ERR_OK);
    const uint32_t spin = 0xeafffffeu; // b .
    assert(uc_mem_write(uc, 0x1000, &spin, sizeof(spin)) == UC_ERR_OK);
    const uc_err err = uc_emu_start(uc, 0x1000, ZeetrisRunner::RETURN_SENTINEL,
                                    0, 16);
    assert(err == UC_ERR_OK); // Unicorn count exhaustion is not an error.
    assert(!ZeetrisRunner::completed_at_return_sentinel(uc, err));

    uint32_t pc = ZeetrisRunner::RETURN_SENTINEL;
    assert(uc_reg_write(uc, UC_ARM_REG_PC, &pc) == UC_ERR_OK);
    assert(ZeetrisRunner::completed_at_return_sentinel(uc, UC_ERR_OK));
    assert(!ZeetrisRunner::completed_at_return_sentinel(uc, UC_ERR_EXCEPTION));
    uc_close(uc);
    return 0;
}
