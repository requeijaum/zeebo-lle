// zeebo_applet_dispatch.h — Bug 4: seleção honesta de manipulador de ciclo de
// vida por módulo. Remove o despacho fixo da Z-Wheel (0x10532344) para módulos
// não relacionados.
//
// Regra de ouro (honestidade dos gates):
//   • Apenas o applet EXPLICITAMENTE rotulado como Z-Wheel (274755) pode usar o
//     manipulador ZeeboApp pré-mapeado em 0:APPS (0x10532344) — preview/harness.
//   • Qualquer outro módulo DEVE usar o próprio manipulador resolvido (entry_va
//     do .mod/ELF), e SOMENTE se ele estiver realmente mapeado/executável.
//   • Módulo com manipulador não resolvido, nulo, ou que "empresta" o handler
//     fixo da Z-Wheel permanece honestamente loaded_only — nunca PASS azul.
#pragma once
#include <cstdint>

namespace zeebo {
namespace applet {

using u32 = uint32_t;

// Manipulador Thumb do ZeeboApp embutido em 0:APPS. Só válido para a Z-Wheel.
static constexpr u32 ZWHEEL_HANDLER_VA = 0x10532344u;

enum DispatchMode : int {
    DISPATCH_REJECT = 0, // loaded_only / falha honesta: sem manipulador para executar
    DISPATCH_ZWHEEL = 1, // ciclo de vida explícito da Z-Wheel (0x10532344)
    DISPATCH_MODULE = 2, // usa o manipulador real, resolvido do próprio módulo
};

struct DispatchDecision {
    DispatchMode mode;
    u32          handler_va;
    const char*  reason;
};

// Decisão pura (sem I/O) de qual manipulador de EVT_APP_START despachar.
//   is_zwheel             : o caller rotulou este applet como a Z-Wheel (274755)?
//   requested_handler_va  : manipulador resolvido do módulo (0 = não resolvido).
//   handler_mapped        : o VA solicitado está mapeado/executável no guest?
inline DispatchDecision select_lifecycle_handler(bool is_zwheel,
                                                  u32  requested_handler_va,
                                                  bool handler_mapped) {
    if (is_zwheel) {
        // Comportamento explícito da Z-Wheel preservado (preview/harness).
        return DispatchDecision{DISPATCH_ZWHEEL, ZWHEEL_HANDLER_VA,
                                "Z-Wheel explícita — manipulador ZeeboApp 0:APPS"};
    }
    // Módulo não-Z-Wheel: JAMAIS forçar/emprestar o manipulador fixo 0x10532344.
    if (requested_handler_va == 0u) {
        return DispatchDecision{DISPATCH_REJECT, 0u,
                                "manipulador do módulo não resolvido — loaded_only"};
    }
    if (requested_handler_va == ZWHEEL_HANDLER_VA) {
        return DispatchDecision{DISPATCH_REJECT, 0u,
                                "módulo não pode emprestar o handler fixo da Z-Wheel — loaded_only"};
    }
    if (!handler_mapped) {
        return DispatchDecision{DISPATCH_REJECT, 0u,
                                "manipulador do módulo não mapeado/executável — loaded_only"};
    }
    return DispatchDecision{DISPATCH_MODULE, requested_handler_va,
                            "manipulador real por módulo"};
}

} // namespace applet
} // namespace zeebo
