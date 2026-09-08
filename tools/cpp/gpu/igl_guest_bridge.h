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
#include <utility>
#include <vector>
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

    // --- Resolução determinística (sem VA hardcoded) --------------------------
    // O 1.1.2_APPS.bin é ELF STRIPPED (shnum=0): gpIGL/gpIEGL NÃO são exports
    // estáticos — são globais preenchidos em runtime pela init EGL/GL do guest.
    // Portanto o único caminho honesto é resolver a vtable a partir do OBJETO de
    // interface vivo. Um objeto BREW IBase-derivado tem `obj[0] = &vtable`, e a
    // vtable é um array de N ponteiros de função, TODOS apontando para segmentos
    // executáveis do firmware. Validamos essa invariante estrutural antes de ligar.

    // Registra os intervalos [lo,hi) de VA executável do firmware (dos program
    // headers com flag X). Usado para validar que cada slot da vtable é código.
    void set_code_ranges(const std::vector<std::pair<u32,u32>>& r) { code_ranges_ = r; }

    // true se `va` (sem o bit Thumb) cai em algum segmento executável conhecido.
    bool is_code_va(u32 va) const {
        u32 v = va & ~1u;
        for (const auto& pr : code_ranges_)
            if (v >= pr.first && v < pr.second) return true;
        return false;
    }

    // Valida estruturalmente uma vtable candidata: lê `slots` ponteiros a partir
    // de vtable_va e exige que uma fração alta (>= min_ratio) aponte para código.
    // Slots nulos são tolerados (funções não implementadas no wrapper). Retorna
    // o número de slots-código; 0 = reprovada. NÃO liga nada (predicado puro).
    int validate_vtable(uc_engine* uc, u32 vtable_va, int slots, double min_ratio = 0.75) const {
        if (!uc || !vtable_va || code_ranges_.empty()) return 0;
        int code = 0, nonnull = 0;
        for (int s = 0; s < slots; ++s) {
            u32 fn = 0;
            if (uc_mem_read(uc, vtable_va + (u32)s * 4, &fn, 4) != UC_ERR_OK) return 0;
            if (fn == 0) continue;
            ++nonnull;
            if (is_code_va(fn)) ++code;
            else return 0; // ponteiro não-nulo que não é código -> não é vtable
        }
        if (nonnull == 0) return 0;
        if ((double)code / (double)nonnull < min_ratio) return 0;
        return code;
    }

    // Resolve gpIGL/gpIEGL a partir do VA de um OBJETO de interface vivo no guest
    // (o ppOut de ISHELL_CreateInstance). Lê obj[0] = vtable_va, valida-a e liga.
    // Retorna true se a vtable foi validada e ligada. Determinístico e honesto:
    // sem endereço inventado, só o que o guest realmente escreveu na memória.
    bool resolve_from_object(uc_engine* uc, u32 obj_va, bool is_igl) {
        if (!uc || !obj_va) return false;
        u32 vtbl = 0;
        if (uc_mem_read(uc, obj_va, &vtbl, 4) != UC_ERR_OK || !vtbl) return false;
        int slots = is_igl ? 80 : 28;
        int good = validate_vtable(uc, vtbl, slots);
        if (!good) {
            printf("[IGL-bridge] resolve_from_object(%s): obj@0x%08x -> vtbl@0x%08x "
                   "REPROVADA (não passou na validação estrutural)\n",
                   is_igl ? "IGL" : "IEGL", obj_va, vtbl);
            return false;
        }
        printf("[IGL-bridge] resolve_from_object(%s): obj@0x%08x -> vtbl@0x%08x "
               "VALIDADA (%d/%d slots-código)\n",
               is_igl ? "IGL" : "IEGL", obj_va, vtbl, good, slots);
        bind_vtable(uc, vtbl, slots, is_igl);
        if (is_igl) igl_vtable_va_ = vtbl; else iegl_vtable_va_ = vtbl;
        return true;
    }

    u32 igl_vtable_va() const { return igl_vtable_va_; }
    u32 iegl_vtable_va() const { return iegl_vtable_va_; }

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
    std::vector<std::pair<u32,u32>> code_ranges_; // segmentos executáveis do firmware
    u32 igl_vtable_va_ = 0;
    u32 iegl_vtable_va_ = 0;
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
