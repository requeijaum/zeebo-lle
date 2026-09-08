// igl_guest_bridge.h — Item 5: liga a vtable gpIGL/gpIEGL do guest ao IglHook.
//
// O `.mod` do jogo despacha 3D via os ponteiros globais gpIGL/gpIEGL (wrapper
// GLES_1x.c/EGL_1x.c da Qualcomm linkado no módulo). Cada entrada da vtable é um
// VA ARM de função. Este bridge intercepta a ENTRADA de qualquer slot dessas
// vtables no Core 0, monta um GuestMachine (arg/read/set_ret sobre uc_mem/regs)
// e despacha para IglHook -> IGpuRasterizer -> framebuffer RGB565 -> sink.
//
// Desacoplado de propósito: IglHook não conhece Unicorn (regra §0). Este header é
// a ÚNICA cola uc<->IglHook, e fica atrás de um gate `guest_running` para não
// disparar durante o boot do kernel.
//
// ABI (igl_hook.h sutileza 2): para slots gl*/egl*, R0 = PRIMEIRO ARG REAL, não
// `po`. arg(n): n<4 -> R0..R3; n>=4 -> [SP + (n-4)*4]. Só AR/Rel/QI usam po-em-R0,
// mas IglHook já trata isso internamente por slot.
#pragma once
#include "igl_hook.h"
#include <cstdint>
#include <unicorn/unicorn.h>

namespace zeebo::gpu {

// Localiza as vtables IGL/IEGL no espaço guest e intercepta suas funções.
class IglGuestBridge {
public:
    explicit IglGuestBridge(IglHook& hook) : hook_(hook) {}

    // Registra a base da vtable IGL (80 slots) e/ou IEGL (28 slots) — os VAs dos
    // ponteiros gpIGL->vtbl / gpIEGL->vtbl no guest. Lê os ponteiros de função
    // e mapeia cada VA de entrada -> (interface, slot) para o dispatch por PC.
    // Se vtable_va==0, a interface é ignorada (honesto: ainda não localizada).
    void bind_igl_vtable(uc_engine* uc, u32 vtable_va, int slots = 80) {
        bind_vtable(uc, vtable_va, slots, /*is_igl=*/true);
    }
    void bind_iegl_vtable(uc_engine* uc, u32 vtable_va, int slots = 28) {
        bind_vtable(uc, vtable_va, slots, /*is_igl=*/false);
    }

    // Habilita o gate: só despacha GL depois que o guest chega a user-space.
    void set_guest_running(bool on) { guest_running_ = on; }
    bool guest_running() const { return guest_running_; }
    bool bound() const { return !fn_map_.empty(); }

    // Chamado pelo hook de código do Core 0 a cada instrução. Se `pc` for a
    // entrada de uma função de vtable conhecida, despacha para IglHook. Retorna
    // true se tratou (o orquestrador deve então retornar da função: PC=LR).
    // NÃO altera PC — devolve a decisão ao caller (desacoplamento).
    bool on_code(uc_engine* uc, u32 pc) {
        if (!guest_running_) return false;
        auto it = fn_map_.find(pc);
        if (it == fn_map_.end()) return false;
        const Target& t = it->second;

        GuestMachine gm;
        gm.arg = [uc](int n) -> u32 {
            u32 v = 0;
            if (n < 4) {
                static const int regs[4] = {UC_ARM_REG_R0, UC_ARM_REG_R1,
                                            UC_ARM_REG_R2, UC_ARM_REG_R3};
                uc_reg_read(uc, regs[n], &v);
            } else {
                u32 sp = 0; uc_reg_read(uc, UC_ARM_REG_SP, &sp);
                uc_mem_read(uc, sp + (u32)(n - 4) * 4, &v, 4);
            }
            return v;
        };
        gm.read = [uc](u32 va, void* dst, u32 size) -> bool {
            return uc_mem_read(uc, va, dst, size) == UC_ERR_OK;
        };
        gm.set_ret = [uc](u32 r0) { uc_reg_write(uc, UC_ARM_REG_R0, &r0); };

        if (t.is_igl) hook_.dispatch_igl(t.slot, gm);
        else          hook_.dispatch_iegl(t.slot, gm);
        return true;
    }

private:
    struct Target { bool is_igl; int slot; };
    IglHook& hook_;
    std::unordered_map<u32, Target> fn_map_;
    bool guest_running_ = false;

    void bind_vtable(uc_engine* uc, u32 vtable_va, int slots, bool is_igl) {
        if (!uc || !vtable_va) return;
        for (int s = 0; s < slots; ++s) {
            u32 fn_va = 0;
            if (uc_mem_read(uc, vtable_va + (u32)s * 4, &fn_va, 4) != UC_ERR_OK) continue;
            if (fn_va == 0) continue; // slot nulo: função não implementada no wrapper
            fn_map_[fn_va] = Target{is_igl, s};
        }
        printf("[IGL-bridge] vtable %s @0x%08x -> %d slots mapeados (%zu funções únicas)\n",
               is_igl ? "IGL" : "IEGL", vtable_va, slots, fn_map_.size());
    }
};

} // namespace zeebo::gpu
