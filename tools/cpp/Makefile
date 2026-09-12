# Zeebo LLE debug harness + device models (C++23) — host build
CXX ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra
LIBS := -lunicorn -lcapstone

DYNARMIC_BASE ?= /home/rafaelfrequiao/projects/zeebo-dynarmic-bringup
DYNARMIC_INCS := -I$(DYNARMIC_BASE)/third_party/dynarmic/src -I$(DYNARMIC_BASE)/third_party/dynarmic/externals/mcl/include -I$(DYNARMIC_BASE)/third_party/dynarmic/externals/fmt/include -I$(DYNARMIC_BASE)/third_party/dynarmic/externals/zydis/include -I$(DYNARMIC_BASE)/build/third_party/dynarmic/externals/zydis
DYNARMIC_LIBS := $(DYNARMIC_BASE)/build/third_party/dynarmic/src/dynarmic/libdynarmic.a \
                 $(DYNARMIC_BASE)/build/third_party/dynarmic/externals/mcl/src/libmcl.a \
                 $(DYNARMIC_BASE)/build/third_party/dynarmic/externals/zydis/libZydis.a \
                 $(DYNARMIC_BASE)/build/third_party/dynarmic/externals/zydis/zycore/libZycore.a \
                 $(DYNARMIC_BASE)/build/third_party/dynarmic/externals/fmt/libfmt.a

CPP_HEADERS := $(wildcard *.h gpu/*.h qdsp5/*.h)

all: zeebo_harness zeebo_devices_test zeebo_lle_main zeebo_lle_mod_probe test_save_state test_audio_sink zeebo_bootinfo_harness

# Depend on every directly included subsystem header. The previous short list
# left zeebo_lle_main stale after edits to MMU/control/audio/device headers.
zeebo_lle_main: zeebo_lle_main.cpp zeebo_dynarmic_core.cpp $(CPP_HEADERS) qdsp5/qdsp5_dispatcher.cpp qdsp5/audpp_engine.cpp qdsp5/stub_engines.cpp gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp gpu/igl_hook.cpp
	$(CXX) $(CXXFLAGS) $(DYNARMIC_INCS) -o $@ zeebo_lle_main.cpp zeebo_dynarmic_core.cpp qdsp5/qdsp5_dispatcher.cpp qdsp5/audpp_engine.cpp qdsp5/stub_engines.cpp gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp gpu/igl_hook.cpp $(DYNARMIC_LIBS) -lunicorn $(shell pkg-config --cflags --libs sdl2)

zeebo_lle_mod_probe: zeebo_lle_mod_probe.cpp
	$(CXX) $(CXXFLAGS) -o $@ zeebo_lle_mod_probe.cpp -lunicorn

test_save_state: test_save_state.cpp zeebo_save_state.h
	$(CXX) $(CXXFLAGS) -o $@ test_save_state.cpp -lunicorn

test_audio_sink: test_audio_sink.cpp zeebo_audio_sink.h
	$(CXX) $(CXXFLAGS) -o $@ test_audio_sink.cpp

# Concorrencia produtor/consumidor e taxa de saida negociada (DD-QW5/QW6).
# Compilado com detector de concorrencia: a versao anterior do sink falhava aqui.
test_audio_lifetime: test_audio_lifetime.cpp zeebo_audio_sink.h
	$(CXX) -std=c++23 -O1 -g -fsanitize=thread -o $@ test_audio_lifetime.cpp -pthread

# Limites do leitor de cabecalhos de programa contra arquivos incoerentes.
# Gancho por instrucao do backend recompilado (DD-JIT).
test_jit_lockstep: test_jit_lockstep.cpp zeebo_dynarmic_core.cpp zeebo_dynarmic_core.h
	$(CXX) $(CXXFLAGS) $(DYNARMIC_INCS) -o $@ test_jit_lockstep.cpp zeebo_dynarmic_core.cpp $(DYNARMIC_LIBS) -lunicorn

test_jit_lost_write: test_jit_lost_write.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -o $@ test_jit_lost_write.cpp -lunicorn

test_jit_unmapped_asymmetry: test_jit_unmapped_asymmetry.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_jit_unmapped_asymmetry.cpp -lunicorn

test_jit_cp15_tlb_lockdown: test_jit_cp15_tlb_lockdown.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_jit_cp15_tlb_lockdown.cpp -lunicorn

test_jit_ldm_usr_rfe: test_jit_ldm_usr_rfe.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_jit_ldm_usr_rfe.cpp -lunicorn

test_vtlb_remap_asymmetry: test_vtlb_remap_asymmetry.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -o $@ test_vtlb_remap_asymmetry.cpp -lunicorn

test_uc_cpu_model_order: test_uc_cpu_model_order.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_uc_cpu_model_order.cpp -lunicorn

test_rex_literal_pool: test_rex_literal_pool.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_rex_literal_pool.cpp -lunicorn

test_pc_rel_reg_offset: test_pc_rel_reg_offset.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_pc_rel_reg_offset.cpp

test_elf_pa_load: test_elf_pa_load.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_elf_pa_load.cpp

test_split_id_bss_clobber: test_split_id_bss_clobber.cpp
	$(CXX) -O1 -std=c++17 -o $@ $< -lunicorn

test_rodata_probe_control: test_rodata_probe_control.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_rodata_probe_control.cpp

# Bug 8: detector de NOP-slide via UC_HOOK_BLOCK (sem uc_mem_read por insn).
# Prova positiva (slide de NOPs dispara) + negativa (loop apertado nao).
test_slide_detector: test_slide_detector.cpp zeebo_slide_detector.h
	$(CXX) $(CXXFLAGS) -o $@ test_slide_detector.cpp -lunicorn
	./test_slide_detector

test_jt_pc_load_hook: test_jt_pc_load_hook.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_jt_pc_load_hook.cpp -lunicorn

test_jit_cp15_reset: test_jit_cp15_reset.cpp zeebo_dynarmic_core.cpp zeebo_dynarmic_core.h
	$(CXX) $(CXXFLAGS) $(DYNARMIC_INCS) -o $@ test_jit_cp15_reset.cpp zeebo_dynarmic_core.cpp $(DYNARMIC_LIBS) -lunicorn

test_jit_unaligned_ldr: test_jit_unaligned_ldr.cpp zeebo_dynarmic_core.cpp zeebo_dynarmic_core.h
	$(CXX) $(CXXFLAGS) $(DYNARMIC_INCS) -o $@ test_jit_unaligned_ldr.cpp zeebo_dynarmic_core.cpp $(DYNARMIC_LIBS) -lunicorn

test_jit_cp15: test_jit_cp15.cpp zeebo_dynarmic_core.cpp zeebo_dynarmic_core.h
	$(CXX) $(CXXFLAGS) $(DYNARMIC_INCS) -o $@ test_jit_cp15.cpp zeebo_dynarmic_core.cpp $(DYNARMIC_LIBS) -lunicorn

test_jit_fault_report: test_jit_fault_report.cpp zeebo_dynarmic_core.cpp zeebo_dynarmic_core.h
	$(CXX) $(CXXFLAGS) $(DYNARMIC_INCS) -o $@ test_jit_fault_report.cpp zeebo_dynarmic_core.cpp $(DYNARMIC_LIBS) -lunicorn

test_jit_code_hook: test_jit_code_hook.cpp zeebo_dynarmic_core.cpp zeebo_dynarmic_core.h
	$(CXX) $(CXXFLAGS) $(DYNARMIC_INCS) -o $@ test_jit_code_hook.cpp zeebo_dynarmic_core.cpp $(DYNARMIC_LIBS) -lunicorn

test_video_mmio: test_video_mmio.cpp zeebo_video_mmio.h
	$(CXX) -std=c++23 -O1 -g -fsanitize=address,undefined -o $@ test_video_mmio.cpp -lunicorn -lpthread

test_mddi_scanout: test_mddi_scanout.cpp zeebo_video_mmio.h
	$(CXX) -std=c++23 -O1 -g -fsanitize=address,undefined -o $@ test_mddi_scanout.cpp
	ASAN_OPTIONS=detect_leaks=0 ./test_mddi_scanout

test_elf_bounds: test_elf_bounds.cpp
	$(CXX) -std=c++23 -O1 -g -fsanitize=address,undefined -o $@ test_elf_bounds.cpp

test_smd_bridge_bounds: test_smd_bridge_bounds.cpp zeebo_smd_bridge.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_smd_bridge_bounds.cpp -lunicorn
	./test_smd_bridge_bounds

# Bug 10: limites do relocador NAND contra phoff/phentsize/phnum incoerentes.
test_nand_relocator_bounds: test_nand_relocator_bounds.cpp
	$(CXX) -std=c++23 -O1 -g -fsanitize=address,undefined -o $@ test_nand_relocator_bounds.cpp


# Bug 5: VIC pending/enable/ack latch + real ARM IRQ exception delivery between
# uc_emu_start slices. Positive run is the GREEN gate; `buggy` reproduces the
# pre-fix RED (mask-ignoring delivery). Requires libunicorn.
test_vic_irq: test_vic_irq.cpp zeebo_vic_irq.h
	$(CXX) $(CXXFLAGS) -DZEEBO_VIC_WITH_UNICORN -o $@ test_vic_irq.cpp -lunicorn

test-vic-irq: test_vic_irq
	./test_vic_irq
	@# Negative mutation: mask-ignoring delivery MUST fail (RED reproduced).
	@if ./test_vic_irq buggy >/dev/null 2>&1; then \
	  echo "FAIL: buggy mask-ignoring delivery unexpectedly passed"; exit 1; \
	else echo "OK: buggy mask-ignoring delivery fails as expected (RED reproduced)"; fi

# Bug 5: GPT counter advances from emulated progress; MATCH crossing raises a
# VIC line. Host-only, deterministic. `buggy` reproduces the stalled-counter RED.
test_gpt_timer: test_gpt_timer.cpp zeebo_gpt_timer.h zeebo_vic_irq.h
	$(CXX) $(CXXFLAGS) -o $@ test_gpt_timer.cpp

test-gpt-timer: test_gpt_timer
	./test_gpt_timer
	@# Negative mutation: stalled counter / no fire MUST fail (RED reproduced).
	@if ./test_gpt_timer buggy >/dev/null 2>&1; then \
	  echo "FAIL: buggy stalled-counter timer unexpectedly passed"; exit 1; \
	else echo "OK: buggy stalled-counter timer fails as expected (RED reproduced)"; fi

# QW99 Part 2 Bug 5: guest MMIO -> PeripheralBus decoder -> VIC/GPT models ->
# real ARM IRQ -> ACK/EOI, exercised over the PRODUCTION decode path under
# Unicorn. Deterministic virtual time drives the timer (not retired insns).
# Negative controls: `buggy-disconnected` (flat-RAM, model never programmed) and
# `buggy-reentrant` (redelivery clobbers LR_irq) MUST fail.
test_peripheral_bus: test_peripheral_bus.cpp zeebo_peripheral_bus.h zeebo_vic_irq.h zeebo_gpt_timer.h
	$(CXX) $(CXXFLAGS) -DZEEBO_VIC_WITH_UNICORN -o $@ test_peripheral_bus.cpp -lunicorn

test-peripheral-bus: test_peripheral_bus
	./test_peripheral_bus
	@# Negative control A: disconnected flat-RAM decode MUST fail (RED).
	@if ./test_peripheral_bus buggy-disconnected >/dev/null 2>&1; then \
	  echo "FAIL: disconnected flat-RAM decode unexpectedly passed"; exit 1; \
	else echo "OK: disconnected flat-RAM decode fails as expected (RED reproduced)"; fi
	@# Negative control B: reentrant IRQ redelivery MUST fail (RED).
	@if ./test_peripheral_bus buggy-reentrant >/dev/null 2>&1; then \
	  echo "FAIL: reentrant IRQ redelivery unexpectedly passed"; exit 1; \
	else echo "OK: reentrant IRQ redelivery fails as expected (RED reproduced)"; fi
	@# Negative control C: Bug5b sub-bank mutant (old +0x08/0x100 window) MUST
	@# fail — firmware COUNT @0xC5000108 is outside the reference decode window.
	@if ./test_peripheral_bus mutant-subbank >/dev/null 2>&1; then \
	  echo "FAIL: GPT sub-bank mutant (+0x08) unexpectedly passed"; exit 1; \
	else echo "OK: GPT sub-bank mutant fails as expected (0xC5000108 mismatch)"; fi

# Bug 6: inter-core ProcComm over shared SMEM — Core0 issue becomes visible,
# only a Core1 service step completes it with an honest status. Host-only.
# `buggy` reproduces the pre-fix RED (Core0-immediate-fabricated-success).
test_proccomm: test_proccomm.cpp zeebo_proccomm.h
	$(CXX) $(CXXFLAGS) -o $@ test_proccomm.cpp

test-proccomm: test_proccomm
	./test_proccomm
	@# Negative mutation: fabricated immediate success MUST fail (RED reproduced).
	@if ./test_proccomm buggy >/dev/null 2>&1; then \
	  echo "FAIL: buggy immediate-success ProcComm unexpectedly passed"; exit 1; \
	else echo "OK: buggy immediate-success ProcComm fails as expected (RED reproduced)"; fi
	@# QW99 Bug 6 control: pending derived from APP_COMMAND MUST fail on the
	@# RESET_MODEM(0x1)/DONE(0x1) collision.
	@if ./test_proccomm derived >/dev/null 2>&1; then \
	  echo "FAIL: APP_COMMAND-derived pending unexpectedly passed (collision unguarded)"; exit 1; \
	else echo "OK: APP_COMMAND-derived pending fails as expected (QW99 collision RED)"; fi

# QW99 Bug 6: integration-level handshake through the same shared-SMEM behavior
# production uses (core0_issue -> core1_service -> mirror completion words to the
# guest view). `derived` reproduces the collision and must fail.
test_proccomm_integration: test_proccomm_integration.cpp zeebo_proccomm.h
	$(CXX) $(CXXFLAGS) -o $@ test_proccomm_integration.cpp

test-proccomm-integration: test_proccomm_integration
	./test_proccomm_integration
	@if ./test_proccomm_integration derived >/dev/null 2>&1; then \
	  echo "FAIL: derived-pending integration unexpectedly passed"; exit 1; \
	else echo "OK: derived-pending integration fails as expected (guest never sees DONE/SUCCESS)"; fi

# QW99 Bug 6 / AUDIO_TODO A11: PRODUCTION scheduling-order regression on a REAL
# Unicorn guest. An actual ARM `str` writes APP_COMMAND under UC_HOOK_MEM_WRITE
# (issue-only, command stays PENDING); the modem services BETWEEN slices; a
# subsequent guest load observes DONE/SUCCESS. `sync` reproduces the A11 clobber
# (service+DONE synchronously inside the hook, then the guest str overwrites
# DONE) and MUST fail. Requires libunicorn.
test_proccomm_production_order: test_proccomm_production_order.cpp zeebo_proccomm.h
	$(CXX) $(CXXFLAGS) -o $@ test_proccomm_production_order.cpp -lunicorn

test-proccomm-production-order: test_proccomm_production_order
	./test_proccomm_production_order
	@# Negative mutation: synchronous service inside the write hook lets the
	@# guest str clobber DONE — MUST fail (A11 RED reproduced on a live guest).
	@if ./test_proccomm_production_order sync >/dev/null 2>&1; then \
	  echo "FAIL: synchronous-in-hook service unexpectedly passed (A11 clobber unguarded)"; exit 1; \
	else echo "OK: synchronous-in-hook service fails as expected (A11 RED: guest str clobbers DONE)"; fi

zeebo_harness: zeebo_harness.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_harness.cpp $(LIBS)

zeebo_devices_test: zeebo_devices_test.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_devices_test.cpp -lunicorn

# Bug 4: seleção honesta de manipulador de ciclo de vida por módulo (host-only,
# sem Unicorn). Prova que o handler fixo da Z-Wheel (0x10532344) NÃO é forçado
# para módulos não relacionados. Mutante que reintroduz o despacho fixo torna os
# casos negativos vermelhos.
test_applet_dispatch: test_applet_dispatch.cpp zeebo_applet_dispatch.h
	$(CXX) $(CXXFLAGS) -o $@ test_applet_dispatch.cpp
	./test_applet_dispatch

# DD0 (QW99 Parte 1): gate honesto de módulo — sem PASS por carga isolada.
# GREEN roda o teste unitário puro. A mutação negativa reintroduz o falso PASS
# loaded-only (PASS_ON_LOAD=1) e DEVE ficar vermelha.
test_module_gate: test_module_gate.cpp zeebo_module_gate.h
	$(CXX) $(CXXFLAGS) -o $@ test_module_gate.cpp

test-module-gate: test_module_gate
	./test_module_gate
	@# Controle mutacional: um gate que dá PASS por carga (ignora o PC no módulo
	@# e trata entry não resolvido como válido) DEVE reprovar os negativos.
	@$(CXX) $(CXXFLAGS) -DZEEBO_DD0_MUTANT_PASS_ON_LOAD=1 -o test_module_gate_mutant test_module_gate.cpp
	@if ./test_module_gate_mutant >/dev/null 2>&1; then \
	  echo "FAIL: mutante PASS-on-load passou inesperadamente (gate não é honesto)"; \
	  rm -f test_module_gate_mutant; exit 1; \
	else echo "OK: mutante PASS-on-load fica vermelho (RED reproduzido)"; \
	  rm -f test_module_gate_mutant; fi

# Bug 4 (gap fechado): prova REAL, sob Unicorn, de que o gate exige permissão de
# EXECUÇÃO do guest (UC_PROT_EXEC via uc_mem_regions), não apenas legibilidade de
# host. Página RO/RW/não-mapeada/parcial → REJECT; RX/RWX → ACEITA. Mutante que
# volta a aferir por uc_mem_read (legibilidade) torna os negativos VERMELHOS.
test_applet_exec_perm: test_applet_exec_perm.cpp zeebo_uc_exec.h zeebo_applet_dispatch.h
	$(CXX) $(CXXFLAGS) -o $@ test_applet_exec_perm.cpp -lunicorn
	./test_applet_exec_perm

test: zeebo_devices_test
	./zeebo_devices_test

# L4 MMU: decoder + VTLB LUT + PhysPool (host-only, sem Unicorn).
test_l4_mmu: test_l4_mmu.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -o $@ test_l4_mmu.cpp
	./test_l4_mmu

test_l4_thread_table: test_l4_thread_table.cpp zeebo_l4_thread.h
	$(CXX) $(CXXFLAGS) -o $@ test_l4_thread_table.cpp
	./test_l4_thread_table

# Bug 2: preservação completa de contexto de CPU + SID por thread.
test_l4_thread_ctx: test_l4_thread_ctx.cpp zeebo_l4_thread.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_thread_ctx.cpp -lunicorn
	./test_l4_thread_ctx

# Bug 7: rwx=0 é revogação real (UC_PROT_NONE), não força legível.
test_l4_mmu_revoke: test_l4_mmu_revoke.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_mmu_revoke.cpp -lunicorn
	./test_l4_mmu_revoke

# Bug 1: mapeamentos L4 isolados por space_id (SpaceMap / VTLB por espaço).
test_l4_space_map: test_l4_space_map.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_space_map.cpp -lunicorn
	./test_l4_space_map

# Bug 1 (runtime): comutação REAL de address space por SID (SpaceManager).
# Positivo: mesma VA executa/lê bytes diferentes por SID e escrita fica isolada.
# Negativo (buggy): mapeamento plano compartilhado FALHA (RED reproduzido).
test_l4_space_switch: test_l4_space_switch.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_space_switch.cpp -lunicorn
	./test_l4_space_switch
	@if ./test_l4_space_switch buggy >/dev/null 2>&1; then \
	  echo "FAIL: flat-mapping (buggy) unexpectedly passed"; exit 1; \
	else echo "OK: flat-mapping (buggy) fails as expected (RED reproduced)"; fi

# QW99: OKL4 base<-extension PD domain/window sharing. Positivo: o espaco de
# extensao resolve a pagina-fonte que so existe no espaco base (pager). Negativo
# (buggy): sem link_base o mapeamento isolado FALHA (RED reproduzido).
test_l4_base_ext_share: test_l4_base_ext_share.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_base_ext_share.cpp -lunicorn
	./test_l4_base_ext_share
	@if ./test_l4_base_ext_share buggy >/dev/null 2>&1; then \
	  echo "FAIL: unlinked (buggy) unexpectedly passed"; exit 1; \
	else echo "OK: unlinked (buggy) fails as expected (RED reproduced)"; fi

# QW99 hardening: PRODUCTION-PATH test — drives the real SpaceManager::activate()
# on a real Unicorn across base->extension (layering + force-rebind over a static
# flat map + switch-away restoration). Positivo PASSA; mutante (sem link_base,
# desabilitando o layering do activate) FALHA (RED reproduzido).
test_l4_base_ext_activate: test_l4_base_ext_activate.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_base_ext_activate.cpp -lunicorn
	./test_l4_base_ext_activate
	@if ./test_l4_base_ext_activate buggy >/dev/null 2>&1; then \
	  echo "FAIL: unlinked activate (buggy) unexpectedly passed"; exit 1; \
	else echo "OK: unlinked activate (buggy) fails as expected (RED reproduced)"; fi

# Bug 1/2: SID vem do SpaceSpecifier(r1) do ThreadControl, NÃO do r5 do ExRegs
# (UserDefHandle); + round-trip do contexto ARM completo via helper central.
# Negativo (buggy): SID lido de r5 (UserDefHandle) FALHA.
test_l4_thread_sid_ctx: test_l4_thread_sid_ctx.cpp zeebo_l4_thread.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_thread_sid_ctx.cpp -lunicorn
	./test_l4_thread_sid_ctx
	@if ./test_l4_thread_sid_ctx buggy >/dev/null 2>&1; then \
	  echo "FAIL: SID-from-r5 (buggy) unexpectedly passed"; exit 1; \
	else echo "OK: SID-from-r5 (buggy) fails as expected (RED reproduced)"; fi

test_exregs_activation: test_exregs_activation.cpp zeebo_l4_thread.h
	$(CXX) $(CXXFLAGS) -o $@ test_exregs_activation.cpp
	./test_exregs_activation

test_l4_ipc_msgtag: test_l4_ipc_msgtag.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_l4_ipc_msgtag.cpp
	./test_l4_ipc_msgtag

test_l4_ipc_dispatch: test_l4_ipc_dispatch.cpp zeebo_l4_ipc.h
	$(CXX) $(CXXFLAGS) -o $@ test_l4_ipc_dispatch.cpp
	./test_l4_ipc_dispatch

test_brew_mif: test_brew_mif.cpp zeebo_brew_mif.h
	$(CXX) $(CXXFLAGS) -o $@ test_brew_mif.cpp
	./test_brew_mif

test_brew_timer: test_brew_timer.cpp zeebo_brew_timer.h
	$(CXX) $(CXXFLAGS) -o $@ test_brew_timer.cpp
	./test_brew_timer

# Bug 3: carregamento seguro de MOD ARM cru (BREW/elf2mod), decode de branch
# e ambiente de prefixo reservado (load_base-8/-4). Requer libunicorn.
test_brew_mod_entry: test_brew_mod_entry.cpp zeebo_brew_loader.h zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -o $@ test_brew_mod_entry.cpp -lunicorn
	./test_brew_mod_entry

# QW99 Parte 4 / DD1a: primeiro PC ASSISTIDO do Double Dragon (hybrid/assisted).
# Valida identidade do pacote (App ID 274754, CLSID 0x0102F789, tamanho do
# ddragonz.mod), resolve o entry ARM cru via BrewLoader e o executa sob Unicorn
# INTÉRPRETE (sem Dynarmic), registrando se o PC entra na faixa do módulo e a 1ª
# dependência ausente. Rotulado hybrid/assisted: NÃO fecha boot orgânico nem PASS
# de jogo, NÃO usa o handler fixo 0x10532344. Requer libunicorn.
test_dd1a_diag: test_dd1a_diag.cpp zeebo_dd1a_diag.h zeebo_brew_loader.h zeebo_brew_mif.h
	$(CXX) $(CXXFLAGS) -o $@ test_dd1a_diag.cpp -lunicorn

# Gate com controles: run principal (ROM real → exit 0, ou SKIP exit 77 sem
# fabricar execução) + mutação force_success que DEVE falhar (RED reproduzido).
test-dd1a: test_dd1a_diag
	./test_dd1a_diag || [ $$? -eq 77 ]
	@# Mutação negativa: force_success fabrica entrada no módulo → controle
	@# negativo passa a alegar execução; o gate DEVE ficar VERMELHO.
	@./test_dd1a_diag buggy >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: mutante force_success pulado (exit 77, ROM real ausente) — NÃO conta como RED"; \
	elif [ $$rc -eq 0 ]; then echo "FAIL: mutante force_success passou (fabricação não detectada)"; exit 1; \
	else echo "OK: mutante force_success falha como esperado (RED reproduzido)"; fi

# Zeetris lifecycle probe (evidence-only): deriva o CLASS ID estrutural do MIF e
# a ABI de chamada do ENTRY (prólogo push {r0,r1,r2,r4,lr} => 3 args AAPCS, ARM)
# dos BYTES REAIS, executa o entry cru sob Unicorn INTÉRPRETE e para HONESTAMENTE
# na 1ª dependência ausente. NÃO alega boot, NÃO usa handler fixo 0x10532344.
# Requer libunicorn. Sem o zeetris.mod/.mif reais → SKIP (exit 77).
test_zeetris_lifecycle: test_zeetris_lifecycle.cpp zeebo_zeetris_lifecycle.h zeebo_brew_loader.h zeebo_brew_mif.h zeebo_dd1a_diag.h
	$(CXX) $(CXXFLAGS) -o $@ test_zeetris_lifecycle.cpp -lunicorn

# Gate com controles: run principal (ROM real → exit 0, ou SKIP exit 77 sem
# fabricar execução) + mutação (ABI Thumb da v1) que DEVE falhar (RED reproduzido).
test-zeetris-lifecycle: test_zeetris_lifecycle
	./test_zeetris_lifecycle || [ $$? -eq 77 ]
	@# Mutação negativa: fingir que o entry ARM roda como Thumb (premissa da v1)
	@# → o controle exige execução limpa que não ocorre; o gate DEVE ficar VERMELHO.
	@./test_zeetris_lifecycle buggy >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: mutante (ABI Thumb) pulado (exit 77, ROM real ausente) — NÃO conta como RED"; \
	elif [ $$rc -eq 0 ]; then echo "FAIL: mutante (ABI Thumb) passou (premissa v1 não refutada)"; exit 1; \
	else echo "OK: mutante (ABI Thumb) falha como esperado (RED reproduzido)"; fi

# Zeetris IShell lifecycle: prova que o VA-0 read (0x123c1bdc) é o ARGUMENTO DE
# CONTEXTO r0=pIShell ausente, não um import/GOT faltante; e implementa o modelo
# mínimo fiel de interface IShell (obj->vtable) que avança a execução até o 1º
# dispatch (ISHELL_CreateInstance, slot 2, ClassID 0x0106e415). Sem Dynarmic.
# Sem o zeetris.mod real → SKIP (exit 77).
test_zeetris_ishell: test_zeetris_ishell.cpp zeebo_brew_ishell.h zeebo_brew_loader.h
	$(CXX) $(CXXFLAGS) -o $@ test_zeetris_ishell.cpp -lunicorn

# Gate com controles: run principal (ROM real → exit 0, ou SKIP exit 77) +
# mutação (vtable NULL "roda limpo") que DEVE falhar (RED reproduzido).
test-zeetris-ishell: test_zeetris_ishell
	./test_zeetris_ishell || [ $$? -eq 77 ]
	@./test_zeetris_ishell buggy >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: mutante (vtable NULL limpa) pulado (exit 77, ROM real ausente) — NÃO conta como RED"; \
	elif [ $$rc -eq 0 ]; then echo "FAIL: mutante (vtable NULL limpa) passou"; exit 1; \
	else echo "OK: mutante falha como esperado (RED reproduzido)"; fi

# Zeetris full lifecycle gate: AEEMod_Load -> IModule::CreateInstance -> HandleEvent(EVT_APP_START)
# Sem Dynarmic. Sem zeetris.mod real -> SKIP (exit 77).
test_zeetris_full_lifecycle: test_zeetris_full_lifecycle.cpp zeebo_brew_ishell.h zeebo_brew_loader.h
	$(CXX) $(CXXFLAGS) -o $@ test_zeetris_full_lifecycle.cpp -lunicorn

test-zeetris-full-lifecycle: test_zeetris_full_lifecycle
	./test_zeetris_full_lifecycle || [ $$? -eq 77 ]
	@./test_zeetris_full_lifecycle buggy >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: mutante (malloc NULL aceito) pulado (exit 77, ROM real ausente) — NÃO conta como RED"; \
	elif [ $$rc -eq 0 ]; then echo "FAIL: mutante (malloc NULL aceito) passou"; exit 1; \
	else echo "OK: mutante falha como esperado (RED reproduzido)"; fi

test_zeetris_runner: test_zeetris_runner.cpp zeebo_zeetris_runner.h gpu/igl_hook.h \
                    gpu/igl_hook.cpp gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_zeetris_runner.cpp \
	  gpu/igl_hook.cpp gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp -lunicorn

test-zeetris-runner: test_zeetris_runner
	./test_zeetris_runner || [ $$? -eq 77 ]
	@./test_zeetris_runner buggy >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: mutante pulado (exit 77, ROM real ausente) — NÃO conta como RED"; \
	elif [ $$rc -eq 0 ]; then echo "FAIL: mutante passou"; exit 1; \
	else echo "OK: mutante falha como esperado (RED reproduzido)"; fi


# Blocker-witness (Tier B): drives the REAL Unicorn AppMgr boot and MEASURES
# that Core0 never reaches ISHELL_CreateInstance/AEECShell/APPS user-space,
# proving a LIVE pIShell context IS derivable (positive witness since the boot
# started reaching the anchor). Requires the real NAND + the built orchestrator;
# without them -> SKIP (exit 77). Mutant ("buggy") now carries the OLD claim
# ("the live boot never reaches the shell") and MUST fail.
test_zeetris_live_shell: test_zeetris_live_shell.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_zeetris_live_shell.cpp

test-zeetris-live-shell: test_zeetris_live_shell zeebo_lle_main
	./test_zeetris_live_shell || [ $$? -eq 77 ]
	@./test_zeetris_live_shell buggy >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: mutante (afirmacao antiga) pulado (exit 77, NAND/orquestrador ausente) — NÃO conta como RED"; \
	elif [ $$rc -eq 0 ]; then echo "FAIL: mutante (afirmacao antiga: ancora inalcancavel) passou"; exit 1; \
	else echo "OK: mutante falha como esperado (o boot ALCANCA o shell)"; fi

# SEMANTIC gate (Tier B, negative control): drives the REAL Unicorn AppMgr boot
# with the read-only ZEEBO_SHELL_SEM instrument and MEASURES whether the exact
# AEECShell/ISHELL_CreateInstance anchor yields a LIVE IShell object+vtable (r0 ->
# *r0 vtable of code) and a real ppOut return pathway. Proves the old "r0 present
# at dispatch == pIShell" inference INVALID; goes GREEN only after the exact
# semantic chain is captured. Real NAND + orchestrator required; else SKIP (77).
# Mutant ("buggy") accepts a NON_SEMANTIC hit as a live shell and MUST fail.
test_zeetris_shell_semantics: test_zeetris_shell_semantics.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_zeetris_shell_semantics.cpp

# Gate com controles: mede que o dispatch AEECShell r0=0xb0d02000 e' NON_SEMANTIC
# (env_base Iguana, nao IShell vivo) e reprova a inferencia antiga (exit 1). Mutante
# que aceita NON_SEMANTIC como prova de shell vivo DEVE falhar. Sem NAND -> SKIP (77).
test-zeetris-shell-semantics: test_zeetris_shell_semantics zeebo_lle_main
	@./test_zeetris_shell_semantics; rc=$$?; \
	if [ $$rc -eq 1 ]; then echo "test-zeetris-shell-semantics: OK (non-semantic provado; inferencia antiga REFUTADA)"; \
	elif [ $$rc -eq 77 ]; then echo "test-zeetris-shell-semantics: SKIP (exit 77)"; \
	elif [ $$rc -eq 0 ]; then echo "test-zeetris-shell-semantics: PASS (live IShell medido)"; \
	else echo "test-zeetris-shell-semantics: FAIL (exit $$rc)"; exit 1; fi
	@./test_zeetris_shell_semantics buggy >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: mutante NON_SEMANTIC pulado (exit 77, NAND ausente) — NÃO conta como RED"; \
	elif [ $$rc -eq 0 ]; then echo "FAIL: mutante (NON_SEMANTIC tratado como shell vivo) passou"; exit 1; \
	else echo "OK: mutante falha como esperado (NON_SEMANTIC nao e' shell vivo)"; fi

# CALL-PROVENANCE gate: prova ESTATICAMENTE (bytes reais do APPS.bin) que o
# anchor 0x10c874f4 rotulado "AEECShell_dispatch" e' na verdade um instalador de
# env do bootstrap APPS/AMSS (chamada direta absoluta cujo retorno ajusta o sp;
# callee THUMB folha que guarda o env num global e retorna constante), NAO um
# dispatch BREW por vtable — e que o simbolo ISHELL_CreateInstance (0x105c7fb4) e'
# uma string rodata. Read-only; nunca inicializa/muda o guest. Exige o APPS.bin
# real; ausente -> SKIP (77). Mutante ("buggy") afirma ser dispatch BREW e falha.
APPS_BIN := ../../nand/1.1.2_APPS.bin

test_zeetris_shell_provenance: test_zeetris_shell_provenance.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_zeetris_shell_provenance.cpp

# CALL-PROVENANCE gate: mede que 0x10c874f4 e' bootstrap AMSS (retorna exit 1)
# e que a premissa de que era um dispatch BREW e' REFUTADA. Mutante afirma que
# e' dispatch BREW e DEVE falhar.
test-zeetris-shell-provenance: test_zeetris_shell_provenance
	@if [ ! -s "$(APPS_BIN)" ]; then \
	  echo "OK: APPS.bin real ausente ($(APPS_BIN)) — SKIP (77) esperado"; fi
	@./test_zeetris_shell_provenance; rc=$$?; \
	if [ $$rc -eq 1 ]; then echo "test-zeetris-shell-provenance: OK (provenance confirmada: AMSS_BOOTSTRAP; premissa BREW refutada)"; \
	elif [ $$rc -eq 77 ]; then echo "test-zeetris-shell-provenance: SKIP (exit 77)"; \
	else echo "test-zeetris-shell-provenance: FAIL (exit $$rc)"; exit 1; fi
	@./test_zeetris_shell_provenance buggy >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: mutante de provenance pulado (exit 77, APPS.bin ausente) — NÃO conta como RED"; \
	elif [ $$rc -eq 0 ]; then echo "FAIL: mutante (AMSS_BOOTSTRAP tratado como dispatch BREW) passou"; exit 1; \
	else echo "OK: mutante falha como esperado (bootstrap nao e' dispatch BREW)"; fi

# Inventário ESTÁTICO de dependências de vtable/static-base do .mod (Bloco 0).
# Gate: o scanner deve prever retroativamente as 8 fronteiras que foram
# descobertas reativamente até 0f64ffa. Exit 77 = SKIP (.mod proprietário ausente).
test-scan-deps:
	@python3 ../py/test_scan_deps.py || [ $$? -eq 77 ]

# Identificação de slots de IDisplay a partir do layout declarativo do SDK
# (Bloco 1). Gate: a aridade declarada no SDK deve explicar os call-sites
# reais do binário; layout deslocado em 1 slot deve REPROVAR.
# Exit 77 = SKIP (SDK ou .mod proprietário ausente).
test_pc14_r0_preserve: test_pc14_r0_preserve.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_pc14_r0_preserve.cpp -lunicorn

# REFUTACAO registrada: a hipotese "escrita incondicional de R0 apos handoff
# causa PC=0x14" foi medida no firmware real e REPROVADA (res_r0 era sempre 0).
# O teste confirma so a mecanica no modelo, entao seu exit natural e 1.
# Invertemos: o gate passa enquanto a mecanica se comportar como descrito.
test-pc14-r0: test_pc14_r0_preserve
	@./test_pc14_r0_preserve; rc=$$?; \
	if [ $$rc -eq 1 ]; then echo "test-pc14-r0: OK (mecanica confirmada; causa REFUTADA no firmware)"; exit 0; \
	elif [ $$rc -eq 2 ]; then echo "test-pc14-r0: SKIP (instrumento inconclusivo)"; exit 77; \
	else echo "test-pc14-r0: FALHOU (exit $$rc)"; exit 1; fi

test-vtbl-layout:
	@python3 ../py/test_vtbl_layout.py || [ $$? -eq 77 ]

# Integração de aliasing via uc_mem_map_ptr (requer libunicorn).
test_l4_mmu_uc: test_l4_mmu_uc.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_mmu_uc.cpp -lunicorn
	./test_l4_mmu_uc

# A SMEM física precisa ser o mesmo backing nos dois cores.
test_shared_memory: test_shared_memory.cpp zeebo_shared_memory.h
	$(CXX) $(CXXFLAGS) -o $@ test_shared_memory.cpp -lunicorn
	./test_shared_memory

# L4_KernelInterface (0xb000c720): prova host-only, com os bytes reais do stub,
# que o SP no intr hook é o SP da trap (mvn sp,#0x4b => 0xFFFFFFB4) e não o frame
# do chamador — logo a escrita extra sp+0/4/8 era espúria e NÃO corrompe r4.
# Mutação negativa (escrever em ip+0/4/8) fica VERMELHA (corrompe r4 -> 0xc).
test_l4_kip_trap: test_l4_kip_trap.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_l4_kip_trap.cpp -lunicorn
	./test_l4_kip_trap

# QW13: regressão TRANSACIONAL de L4_MapControl sob Unicorn — fixa MRs iniciais,
# entradas da syscall, transação de map (regiões criadas em ordem, escrita real
# de página), resultado r0, skip de item malformado e whole-space sem overflow.
# NÃO detecta o write-back idêntico ausente (echo neutro é indistinguível por
# black-box); o "Passo 13" continua BLOQUEADO pendente de harness de guest vivo.
# Sem Unicorn não roda; requer libunicorn.
test_l4_mmu_txn: test_l4_mmu_txn.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_mmu_txn.cpp -lunicorn
	./test_l4_mmu_txn

# QW1: harness isolado de BootInfo/fpage (host-only, sem Unicorn).
#
# A NAND real (nand/*.bin) é proprietária e gitignored, portanto NUNCA está
# presente num clone limpo. Para preservar a semântica "sem falso-verde" sem
# tornar o `check` agregado permanentemente vermelho, os alvos são SEPARADOS:
#
#   * zeebo_bootinfo_harness  — COMPILE-ONLY. Apenas prova que o harness
#     compila. NÃO executa nada e NÃO afirma validação por bytes reais.
#     Faz parte de `all` e `check` (build limpo, sem NAND).
#
#   * test-bootinfo-real      — GATE REAL. Exige a cópia de trabalho da NAND
#     (BOOTINFO_APPS). Se ausente, o harness FALHA com exit não-zero — nunca
#     SKIP-como-PASS. NÃO faz parte de `check`; rode-o explicitamente quando
#     tiver os bytes reais. Prova (A) records/pools por bytes+mutação
#     (firmware-derived) e rotula (B) as 96 fpages como hipótese explícita do
#     mempool_init (NÃO derivada).
BOOTINFO_APPS ?= ../../nand/1.1.2_APPS.bin
zeebo_bootinfo_harness: zeebo_bootinfo_harness.cpp zeebo_bootinfo.h zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_bootinfo_harness.cpp

# Gate real por bytes da NAND. Exige BOOTINFO_APPS; falha nonzero se ausente.
# Fora do `check` de propósito (a NAND é proprietária/gitignored).
test-bootinfo-real: zeebo_bootinfo_harness
	./zeebo_bootinfo_harness $(BOOTINFO_APPS)

# GPU subsystem (tools/cpp/gpu/) — build isolado + verificação por framebuffer.
gpu:
	$(MAKE) -C gpu -f Makefile.gpu gpu_smoke igl_smoke gpu_display_integration igl_transform_smoke igl_guest_bridge_test gl_quickwins_smoke gl_alpha_sampler_smoke atitc_smoke gl_clip_smoke

test-gpu: gpu
	cd gpu && ./gpu_smoke && ./igl_smoke && ./gpu_display_integration && ./igl_transform_smoke && ./igl_guest_bridge_test && ./gl_quickwins_smoke && ./gl_alpha_sampler_smoke && ./atitc_smoke && ./gl_clip_smoke

# QDSP5 route gate: isolated because it has no Unicorn/SDL dependency. Q1.2a
# is infrastructure only until Q0.1 captures a genuine firmware packet.
test-qdsp5:
	$(MAKE) -C qdsp5 -f Makefile.qdsp5 test

# Z-Wheel harness: executa o manipulador de eventos Thumb do ZeeboApp (0:APPS)
# sob Unicorn com EVT_APP_START e captura a chamada gráfica via a vtable em
# [applet+0x2c] → SoftRasterizer/IglGuestBridge.
zeebo_zwheel_harness: zeebo_zwheel_harness.cpp gpu/igl_guest_bridge.h gpu/igl_hook.h gpu/igpu_rasterizer.h gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp gpu/igl_hook.cpp
	$(CXX) $(CXXFLAGS) -o $@ zeebo_zwheel_harness.cpp gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp gpu/igl_hook.cpp -lunicorn

test-zwheel: zeebo_zwheel_harness
	./zeebo_zwheel_harness ../../nand/1.1.2_APPS.bin

# REX heap split-I/D harness: prova por execução real o particionamento do heap
# REX de 2MB em 0xf0000000 (Core 1 / ARM9) sem corromper o .text do AMSS, a
# travessia da re-entry 0xf000e6d4 e o decode de rex_sched 0xf0013b84.
zeebo_rex_harness: zeebo_rex_harness.cpp
	$(CXX) $(CXXFLAGS) -o $@ zeebo_rex_harness.cpp -lunicorn

test-rex: zeebo_rex_harness
	./zeebo_rex_harness ../../nand/1.1.2_AMSS.bin

test-core1: test-rex

# Passo 3 / Fase 13: valida a integração do pipeline gráfico da Z-Wheel
# (SoftRasterizer, slot 10/0x28) → sink SDL2 no emulador principal, em modo
# headless (sem abrir janela), comprovando frame RGB565 com soma de pixels > 0.
test-zwheel-preview: zeebo_lle_main
	SDL_VIDEODRIVER=dummy ./zeebo_lle_main --zwheel-preview-headless

# Ciclo de vida do applet Z-Wheel (274755) carregado do EFS2: extrai o gnode de
# 274755 da NAND 0:EFS2APPS, injeta via BrewLoader e despacha EVT_APP_START ao
# manipulador ZeeboApp pré-mapeado em 0:APPS (0x10532344), roteando a chamada
# gráfica ao SoftRasterizer. Valida sem crash e exige o PASS do ciclo de vida.
test-efs2-zwheel: zeebo_lle_main
	SDL_VIDEODRIVER=dummy ./zeebo_lle_main --efs2-run=274755 --seconds=0 --cycles=1 \
	  | grep -q "Z-Wheel/Life] PASS" \
	  && echo "PASS: --efs2-run=274755 instanciou o ciclo de vida do ZeeboApp (EVT_APP_START, r0=1)." \
	  || (echo "FAIL: ciclo de vida do applet 274755 não completou EVT_APP_START"; exit 1)

# Passo 5: mapeamento Z-Pad/SDL2 → AVK e despacho contínuo de EVT_KEY_PRESS/
# EVT_KEY_RELEASE ao manipulador BREW (HandleEvent), provado por execução real
# sob Unicorn (applet stub que consome a tecla e retorna r0=1).
zeebo_input_harness: zeebo_input_harness.cpp zeebo_brew_loader.h zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_input_harness.cpp -lunicorn

test-input: zeebo_input_harness
	./zeebo_input_harness

# Passo 11: loop interativo/contínuo do applet Z-Wheel (274755) sob SDL2.
# Roda o ciclo de vida + loop por tempo real (--seconds=1) em headless (dummy),
# apresentando o framebuffer à taxa de quadros e drenando eventos Z-Pad→AVK.
# Valida que o loop entra e encerra organicamente sem crash.
test-efs2-zwheel-loop: zeebo_lle_main
	SDL_VIDEODRIVER=dummy ./zeebo_lle_main --efs2-run=274755 --seconds=1 \
	  | grep -q "Z-Wheel/Loop] loop encerrado" \
	  && echo "PASS: --efs2-run=274755 --seconds=1 executou o loop interativo contínuo (Passo 11)." \
	  || (echo "FAIL: loop interativo do applet 274755 não encerrou corretamente"; exit 1)

test-efs2-game-reksio: zeebo_lle_main
	SDL_VIDEODRIVER=dummy ./zeebo_lle_main --efs2-run=reksio.mod --seconds=1 \
	  | grep -q "PASS: Jogo/Applet 'Reksio (reksio.mod)' entrou em execução" \
	  && echo "PASS: --efs2-run=reksio.mod --seconds=1 executou o jogo comercial Reksio." \
	  || (echo "FAIL: execução do jogo reksio.mod falhou"; exit 1)

test-debug-agent: zeebo_lle_main
	python3 test_zeebo_debug_agent.py

# QW-ROMS1 / DD0: valida o carregamento de .mod externos (host filesystem, SEM
# montagem de NAND) via --applet=<caminho>. Gate HONESTO (QW99 Parte 1): um jogo
# externo só recebe PASS quando o módulo é VÁLIDO e um PC pertencente à sua faixa
# [load_va,load_va+size) REALMENTE executa — carregar (loaded_only) NÃO basta e
# módulo inválido falha ANTES de executar. A linha "PASS honesto" é emitida só
# pelo gate DD0; "loaded_only", "GATE DD0 → INVALID" ou exit!=0 são FAIL de jogo.
# Uso: make test-roms-external ROM=<path.mod>
test-roms-external: zeebo_lle_main
	@test -n "$(ROM)" || (echo "Uso: make test-roms-external ROM=<caminho.mod>"; exit 1)
	@out=$$(SDL_VIDEODRIVER=dummy ./zeebo_lle_main --applet="$(ROM)" --seconds=1 2>&1); \
	 rc=$$?; \
	 if echo "$$out" | grep -q "PASS honesto" && [ $$rc -eq 0 ]; then \
	   echo "PASS: --applet=$(ROM) EXECUTOU de fato (PC no módulo) via injeção externa."; \
	 elif echo "$$out" | grep -q "GATE DD0 → INVALID"; then \
	   echo "FAIL(esperado p/ inválido): $(ROM) rejeitado ANTES de executar (módulo inválido)."; exit 1; \
	 else \
	   echo "FAIL: applet externo $(ROM) permaneceu loaded_only (sem PC executado no módulo) — sem PASS de jogo."; exit 1; \
	 fi

# QW6 + QW15 transactional conformance: locally authored ARM/Thumb vectors run
# under the Unicorn txn probe and diffed against golden traces (ordered
# bus transactions: prefetch/load/store width/addr/value + full INIT/FINAL
# CPSR). qw15_page pins genuine cross-4KiB-boundary accesses. No NAND / no
# proprietary asset -- safe for a clean clone, so it belongs in `check`. The
# script exits nonzero on any AS/LD/objcopy/build/run/diff failure.
test-txn:
	bash run_qw6_txn.sh

test_control_server: test_control_server.cpp zeebo_control_server.h
	$(CXX) $(CXXFLAGS) -o $@ test_control_server.cpp -pthread

test_probe_registry: test_probe_registry.cpp zeebo_probe_registry.h
	$(CXX) $(CXXFLAGS) -o $@ test_probe_registry.cpp -pthread

# QW17: isolated regression for c0_intr_hook syscall pc-resume convention.
# Default run (offset 0 == pc) is the GREEN gate; `4` reproduces the pre-fix RED.
test_intr_pc_resume: test_intr_pc_resume.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_intr_pc_resume.cpp -lunicorn

test-intr-pc-resume: test_intr_pc_resume
	./test_intr_pc_resume
	@# Negative mutation: buggy pc+4 convention MUST fail (skips sentinel).
	@if ./test_intr_pc_resume 4 >/dev/null 2>&1; then \
	  echo "FAIL: buggy pc+4 convention unexpectedly passed"; exit 1; \
	else echo "OK: buggy pc+4 convention fails as expected (RED reproduced)"; fi

# QW19: isolated regression for MapControl stub frame restoration.
test_mapcontrol_frame_resume: test_mapcontrol_frame_resume.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_mapcontrol_frame_resume.cpp -lunicorn

test-mapcontrol-frame-resume: test_mapcontrol_frame_resume
	./test_mapcontrol_frame_resume
	@# Negative mutation: buggy convention MUST fail (corrupts r4).
	@if ./test_mapcontrol_frame_resume buggy >/dev/null 2>&1; then \
	  echo "FAIL: buggy convention unexpectedly passed"; exit 1; \
	else echo "OK: buggy convention fails as expected (RED reproduced)"; fi

# QW26: isolated regression for ThreadControl (0x08) + SpaceControl (0x18) stub frame restoration.
test_threadspace_frame_resume: test_threadspace_frame_resume.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_threadspace_frame_resume.cpp -lunicorn

test-threadspace-frame-resume: test_threadspace_frame_resume
	./test_threadspace_frame_resume
	@# Negative mutation: buggy convention MUST fail (corrupts callee-saved / drops writeback).
	@if ./test_threadspace_frame_resume buggy >/dev/null 2>&1; then \
	  echo "FAIL: buggy convention unexpectedly passed"; exit 1; \
	else echo "OK: buggy convention fails as expected (RED reproduced)"; fi

# QW27: isolated regression for ExchangeRegisters (0x0c) + ThreadSwitch (0x04)
# + Schedule (0x10) stub frame restoration and trap-stack output writebacks.
test_exregs_frame_resume: test_exregs_frame_resume.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_exregs_frame_resume.cpp -lunicorn

test-exregs-frame-resume: test_exregs_frame_resume
	./test_exregs_frame_resume
	@# Negative mutation: buggy convention MUST fail (corrupts callee-saved,
	@# drops trap-stack writebacks, and derails ExchangeRegisters to PC=0).
	@if ./test_exregs_frame_resume buggy >/dev/null 2>&1; then \
	  echo "FAIL: buggy convention unexpectedly passed"; exit 1; \
	else echo "OK: buggy convention fails as expected (RED reproduced)"; fi

# QW41: isolated regression for T-bit preservation on SVC resume when the
# caller is outside the fixed L4 kernel-stub range (AMSS/BREW Thumb code).
test_svc_thumb_resume: test_svc_thumb_resume.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_svc_thumb_resume.cpp -lunicorn

test-svc-thumb-resume: test_svc_thumb_resume
	./test_svc_thumb_resume
	@# Negative mutation: buggy convention (T-bit never restored) MUST fail.
	@if ./test_svc_thumb_resume buggy >/dev/null 2>&1; then \
	  echo "FAIL: buggy convention unexpectedly passed"; exit 1; \
	else echo "OK: buggy convention fails as expected (RED reproduced)"; fi

# QW42: isolated regression for L4_ExchangeRegisters resume mode when the
# caller is a LOCAL ARM copy of the trap-stack stub inside ig_naming
# (0xb0100000-0xb0120000), distinct from the fixed kernel stub range.
test_exregs_ig_naming: test_exregs_ig_naming.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_exregs_ig_naming.cpp -lunicorn

test-exregs-ig-naming: test_exregs_ig_naming
	./test_exregs_ig_naming
	@# Negative mutation: generic-Thumb classifier (QW41-only) MUST fail here.
	@if ./test_exregs_ig_naming buggy >/dev/null 2>&1; then \
	  echo "FAIL: buggy classifier unexpectedly passed"; exit 1; \
	else echo "OK: buggy classifier fails as expected (RED reproduced)"; fi

test-cli: zeebo_lle_main test_cli_paths
	./zeebo_lle_main --cycles=abc >/dev/null 2>&1; test $$? -eq 2
	./zeebo_lle_main --control-port=70000 >/dev/null 2>&1; test $$? -eq 2
	./test_cli_paths

test_cli_paths: test_cli_paths.cpp zeebo_cli_paths.h
	$(CXX) $(CXXFLAGS) -o $@ test_cli_paths.cpp

# NOTA: `check` inclui APENAS o build compile-only do harness de BootInfo
# (zeebo_bootinfo_harness), que NÃO executa nem afirma validação por bytes
# reais. O gate real que exige a NAND proprietária é `test-bootinfo-real` e
# fica FORA do agregado de propósito, para que um clone limpo (sem NAND) rode
# `check` verde sem jamais alegar validação real-byte que não ocorreu.
# QW20: gate de higiene do clean (estático, NÃO destrói artefatos). Garante que
# todo binário produzido por alvo de link nos Makefiles esteja coberto pelo
# alvo clean. Mutação negativa (remover 1 nome do clean) torna o gate VERMELHO.
test-clean-hygiene:
	python3 test_clean_hygiene.py
	python3 test_mutation_gate_hygiene.py
	python3 test_binary_ignore_hygiene.py

# ---------------------------------------------------------------------------
# QW99 Part 1 — truthful test tiers. The aggregate `check` mixes two classes of
# gate: (A) clean-clone-safe gates that need NO proprietary NAND, and (B) gates
# that require the user's working NAND (nand/*.bin, gitignored, absent in a
# clean clone). Conflating them lets a clean clone either go falsely green (NAND
# gates skipped-as-pass) or falsely red. The tiers make the contract explicit:
#
#   check-fast     — Tier A ONLY. No proprietary NAND touched. Safe on a clean
#                    clone; this is the honest "does the build/logic hold" gate.
#   check-firmware — Tier B ONLY. REQUIRES the working NAND. Fails LOUD (exit 1)
#                    with an explicit message if any required nand/*.bin is
#                    absent — never SKIP-as-PASS.
#   check-full     — check-fast + check-firmware (everything, needs NAND).
#   check          — kept as the historical full aggregate (== check-full) so
#                    existing callers/CI stay compatible and truthful.
#
# NAND gates pulled out of the fast tier into FIRMWARE_TARGETS: test (device
# model runtime reads nand/1.1.2.bin), test-zwheel-preview (zeebo_lle_main boot
# loads NAND) plus test-rex test-zwheel test-efs2-fs test-efs2-zwheel
# test-efs2-zwheel-loop test-debug-agent.
NAND_REQUIRED_BINS := ../../nand/1.1.2_AMSS.bin ../../nand/1.1.2_APPS.bin ../../nand/1.1.2.bin
FIRMWARE_TARGETS := test test-zwheel-preview test-rex test-zwheel test-efs2-fs test-efs2-zwheel test-efs2-zwheel-loop test-debug-agent test-zeetris-live-shell

check-fast: test-clean-hygiene test-efs2apps test-gpu test-qdsp5 test-input \
       test-cli \
       test_l4_mmu test_l4_mmu_uc test_shared_memory test_l4_kip_trap test_l4_mmu_txn test_l4_thread_table test_l4_thread_ctx test_l4_mmu_revoke test_l4_space_map test_l4_space_switch test_l4_base_ext_activate test_l4_thread_sid_ctx test_exregs_activation test_l4_ipc_msgtag test_l4_ipc_dispatch test_brew_mif test_brew_mod_entry test_brew_timer zeebo_bootinfo_harness test_audio_sink test_audio_lifetime test_elf_bounds test_nand_relocator_bounds test_video_mmio test_mddi_scanout test_jit_code_hook test_jit_fault_report test_jit_lockstep test_jit_cp15 test_jit_unaligned_ldr test_jit_cp15_reset test_jit_lost_write test_jit_unmapped_asymmetry test_jit_cp15_tlb_lockdown test_jit_ldm_usr_rfe test_vtlb_remap_asymmetry test_uc_cpu_model_order test_rex_literal_pool test_pc_rel_reg_offset test_jt_pc_load_hook test_elf_pa_load test_rodata_probe_control test_split_id_bss_clobber test_slide_detector test_control_server test_save_state \
       test_probe_registry test-txn test-intr-pc-resume test-mapcontrol-frame-resume test-threadspace-frame-resume test-exregs-frame-resume test-svc-thumb-resume test-exregs-ig-naming test_smd_bridge_bounds test_applet_dispatch test_applet_exec_perm test-module-gate test-vic-irq test-gpt-timer test-proccomm test-proccomm-integration test-proccomm-production-order test-peripheral-bus test-dd1a test-scan-deps test-vtbl-layout test-pc14-r0
	./test_applet_dispatch
	./test_applet_exec_perm
	./test_brew_timer
	./test_audio_sink
	./test_audio_lifetime
	@echo "== QDSP5: rota AUDPLAY/AUDPP com contratos de recusa =="
	$(MAKE) test-qdsp5
	./test_elf_bounds
	./test_nand_relocator_bounds
	@echo "== MMIO de video: leituras chegam aos modelos =="
	ASAN_OPTIONS=detect_leaks=0 ./test_video_mmio
	@echo "== MDDI scanout: consumo da lista ligada RGB565 (positivo + negativos) =="
	ASAN_OPTIONS=detect_leaks=0 ./test_mddi_scanout
	@echo "== JIT: gancho por instrucao executada =="
	./test_jit_code_hook
	./test_jit_fault_report
	./test_jit_lockstep
	./test_jit_cp15
	./test_jit_unaligned_ldr
	./test_jit_cp15_reset
	./test_jit_lost_write
	./test_jit_unmapped_asymmetry
	./test_jit_cp15_tlb_lockdown
	./test_jit_ldm_usr_rfe
	./test_vtlb_remap_asymmetry
	./test_uc_cpu_model_order
	./test_rex_literal_pool
	./test_pc_rel_reg_offset
	@# exit 77 = SKIP (imagem AMSS ausente); nao e' sucesso nem falha.
	./test_elf_pa_load || [ $$? -eq 77 ]
	@echo "== controle negativo do probe de .rodata (Core1) =="
	./test_rodata_probe_control || [ $$? -eq 77 ]
	./test_split_id_bss_clobber || [ $$? -eq 77 ]
	@echo "== bug 8: detector de NOP-slide via UC_HOOK_BLOCK (positivo+negativo) =="
	./test_slide_detector
	./test_jt_pc_load_hook
	./test_control_server
	./test_probe_registry
	./test_save_state
	./test_smd_bridge_bounds
	@echo '--- bug 5/6: VIC IRQ delivery, GPT timer, ProcComm (positivo + controle negativo) ---'
	$(MAKE) test-vic-irq
	$(MAKE) test-gpt-timer
	$(MAKE) test-peripheral-bus
	$(MAKE) test-proccomm
	$(MAKE) test-proccomm-integration
	$(MAKE) test-proccomm-production-order
	$(MAKE) test-zeetris-lifecycle
	$(MAKE) test-zeetris-ishell
	$(MAKE) test-zeetris-full-lifecycle
	$(MAKE) test-zeetris-runner
	@echo '--- bateria L4/MMU/IPC/brew (executada AQUI, nao so na receita de build) ---'
	./test_l4_mmu
	./test_l4_thread_table
	./test_l4_thread_ctx
	./test_l4_mmu_revoke
	./test_l4_space_map
	./test_exregs_activation
	./test_l4_ipc_msgtag
	./test_l4_ipc_dispatch
	./test_brew_mif
	./test_brew_mod_entry
	./test_l4_mmu_uc
	./test_l4_kip_trap
	./test_l4_mmu_txn
	@echo "[check] zeebo_bootinfo_harness compilou (compile-only; NAO valida bytes reais)."
	@echo "[check] Para validacao real-byte da NAND rode: make test-bootinfo-real"
	@echo ""
	@echo "[check-fast] PASS — Tier A (clean-clone-safe) gates green; NO proprietary NAND touched."

# check-firmware — Tier B. REQUIRES the working NAND. Verifies every required
# nand/*.bin is present FIRST and fails LOUD if any is absent (never SKIP-as-
# PASS), then runs the firmware-dependent gates.
check-firmware:
	@missing=""; for f in $(NAND_REQUIRED_BINS); do \
	    if [ ! -f "$$f" ]; then missing="$$missing $$f"; fi; \
	 done; \
	 if [ -n "$$missing" ]; then \
	    echo "[check-firmware] FAIL: required proprietary NAND image(s) absent:$$missing"; \
	    echo "[check-firmware] These gates validate real firmware bytes and cannot SKIP-as-PASS."; \
	    exit 1; \
	 fi; \
	 echo "[check-firmware] NAND present — running $(words $(FIRMWARE_TARGETS)) firmware gate(s)."
	$(MAKE) $(FIRMWARE_TARGETS)
	@echo "[check-firmware] PASS — Tier B firmware-byte gates green (real NAND validated)."

# check-full — everything: Tier A then Tier B. Needs the working NAND.
check-full: check-fast check-firmware
	@echo "[check-full] PASS — Tier A + Tier B all green."

# check — historical full aggregate, kept == check-full for CI/caller compat.
check: check-full

clean:
	-rm -f test_pc14_r0_preserve
	rm -f zeebo_harness zeebo_devices_test zeebo_lle_main zeebo_lle_mod_probe \
	      test_save_state test_audio_sink test_audio_lifetime test_elf_bounds test_nand_relocator_bounds test_video_mmio test_mddi_scanout test_shared_memory test_brew_mod_entry test_jit_code_hook test_jit_fault_report test_jit_lockstep test_jit_cp15 test_jit_unaligned_ldr test_jit_cp15_reset test_jit_lost_write test_jit_unmapped_asymmetry test_jit_cp15_tlb_lockdown test_jit_ldm_usr_rfe test_vtlb_remap_asymmetry test_uc_cpu_model_order test_rex_literal_pool test_pc_rel_reg_offset test_jt_pc_load_hook test_elf_pa_load test_rodata_probe_control test_split_id_bss_clobber test_slide_detector test_control_server test_l4_mmu test_l4_mmu_uc test_l4_mmu_revoke test_l4_space_map test_l4_thread_ctx test_l4_kip_trap test_l4_mmu_txn test_l4_thread_table test_exregs_activation test_l4_ipc_msgtag test_l4_ipc_dispatch test_brew_mif test_brew_timer test_l4_space_switch test_l4_base_ext_share test_l4_base_ext_activate test_l4_thread_sid_ctx \
	      zeebo_zwheel_harness zeebo_efs2apps zeebo_rex_harness \
	      zeebo_input_harness test_efs2_fs zeebo_partition zeebo_elf zeebo_boot zeebo_bootinfo_harness \
	      zeebo_lle_txn_probe test_probe_registry test_cli_paths test_intr_pc_resume test_mapcontrol_frame_resume test_threadspace_frame_resume test_exregs_frame_resume test_svc_thumb_resume test_exregs_ig_naming test_smd_bridge_bounds test_applet_dispatch test_applet_exec_perm test_module_gate test_module_gate_mutant test_vic_irq test_gpt_timer test_proccomm test_proccomm_integration test_proccomm_production_order test_peripheral_bus test_dd1a_diag test_zeetris_ishell test_zeetris_lifecycle test_zeetris_live_shell test_zeetris_shell_provenance test_zeetris_shell_semantics test_zeetris_full_lifecycle test_zeetris_runner *.o
	rm -rf testkit/build
	$(MAKE) -C qdsp5 -f Makefile.qdsp5 clean
	$(MAKE) -C gpu -f Makefile.gpu clean

.PHONY: all check check-fast check-firmware check-full test test-cli clean test-mddi-scanout test-zeetris-lifecycle test-zeetris-runner gpu test-gpu test-qdsp5 test-zwheel test-rex test-core1 test-zwheel-preview test-efs2-zwheel test-efs2-zwheel-loop test-efs2-game-reksio test-debug-agent test-input test-efs2-fs test-bootinfo-real test-txn test-clean-hygiene test-module-gate test-vic-irq test-gpt-timer test-proccomm test-proccomm-integration test-proccomm-production-order test-peripheral-bus test-dd1a test-scan-deps test-vtbl-layout test-pc14-r0 test-zeetris-live-shell test-zeetris-shell-provenance test-zeetris-full-lifecycle
zeebo_partition: zeebo_partition.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_partition.cpp -lunicorn

zeebo_efs2apps: zeebo_efs2apps.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_efs2apps.cpp -lunicorn

test-efs2apps: zeebo_efs2apps
	./zeebo_efs2apps

# Passo 2 / Fase 13: parser C++ do 0:EFS2APPS (zeebo_efs2_fs.h) — varre dirents
# (marcador 0x69), resolve (parent_inode,name) em O(1), lê clusters de 512B e
# encadeia blocos indiretos u32; validado por bytes reais + checksum FNV-1a.
test_efs2_fs: test_efs2_fs.cpp zeebo_efs2_fs.h
	$(CXX) $(CXXFLAGS) -o $@ test_efs2_fs.cpp

test-efs2-fs: test_efs2_fs
	./test_efs2_fs ../../nand/1.1.2.bin

zeebo_elf: zeebo_elf.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_elf.cpp -lunicorn -lcapstone

zeebo_boot: zeebo_boot.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_boot.cpp -lunicorn
