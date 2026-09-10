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
	$(CXX) $(CXXFLAGS) -o $@ test_jit_lost_write.cpp

test_jit_unmapped_asymmetry: test_jit_unmapped_asymmetry.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_jit_unmapped_asymmetry.cpp -lunicorn

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

test_elf_bounds: test_elf_bounds.cpp
	$(CXX) -std=c++23 -O1 -g -fsanitize=address,undefined -o $@ test_elf_bounds.cpp

zeebo_harness: zeebo_harness.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_harness.cpp $(LIBS)

zeebo_devices_test: zeebo_devices_test.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_devices_test.cpp -lunicorn

test: zeebo_devices_test
	./zeebo_devices_test

# L4 MMU: decoder + VTLB LUT + PhysPool (host-only, sem Unicorn).
test_l4_mmu: test_l4_mmu.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -o $@ test_l4_mmu.cpp
	./test_l4_mmu

test_l4_thread_table: test_l4_thread_table.cpp zeebo_l4_thread.h
	$(CXX) $(CXXFLAGS) -o $@ test_l4_thread_table.cpp
	./test_l4_thread_table

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

# Integração de aliasing via uc_mem_map_ptr (requer libunicorn).
test_l4_mmu_uc: test_l4_mmu_uc.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_mmu_uc.cpp -lunicorn
	./test_l4_mmu_uc

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
	$(MAKE) -C gpu -f Makefile.gpu gpu_smoke igl_smoke gpu_display_integration igl_transform_smoke igl_guest_bridge_test gl_quickwins_smoke gl_alpha_sampler_smoke atitc_smoke

test-gpu: gpu
	cd gpu && ./gpu_smoke && ./igl_smoke && ./gpu_display_integration && ./igl_transform_smoke && ./igl_guest_bridge_test && ./gl_quickwins_smoke && ./gl_alpha_sampler_smoke && ./atitc_smoke

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

# QW-ROMS1: valida o carregamento de .mod externos (host filesystem, SEM
# montagem de NAND) via --applet=<caminho> para o catálogo real de ROMs de
# Zeebo (mídia física do usuário, fora do repo — ver ROADMAP para o path).
# Cada jogo deve completar EVT_APP_START (r0=1) e o dispatch de ciclo de vida
# (PASS), provando que o injetor BrewLoader funciona como VFS mínimo: qualquer
# .mod válido pode ser injetado diretamente em 0x12000000 sem precisar estar
# catalogado na NAND 1.1.2 (EFS2). Uso: make test-roms-external ROM=<path.mod>
test-roms-external: zeebo_lle_main
	@test -n "$(ROM)" || (echo "Uso: make test-roms-external ROM=<caminho.mod>"; exit 1)
	SDL_VIDEODRIVER=dummy ./zeebo_lle_main --applet="$(ROM)" --seconds=1 \
	  | grep -q "BREW/Applet] PASS" \
	  && echo "PASS: --applet=$(ROM) executou via injeção externa (sem NAND)." \
	  || (echo "FAIL: applet externo $(ROM) não completou o ciclo de vida"; exit 1)

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

check: test-clean-hygiene test test-zwheel-preview test-rex test-zwheel test-efs2apps test-gpu test-input \
       test-efs2-fs test-efs2-zwheel test-efs2-zwheel-loop test-debug-agent test-cli \
       test_l4_mmu test_l4_mmu_uc test_l4_kip_trap test_l4_mmu_txn test_l4_thread_table test_exregs_activation test_l4_ipc_msgtag test_l4_ipc_dispatch test_brew_mif test_brew_timer zeebo_bootinfo_harness test_audio_sink test_audio_lifetime test_elf_bounds test_video_mmio test_jit_code_hook test_jit_lost_write test_jit_unmapped_asymmetry test_control_server test_save_state \
       test_probe_registry test-txn test-intr-pc-resume test-mapcontrol-frame-resume test-threadspace-frame-resume test-exregs-frame-resume test-svc-thumb-resume test-exregs-ig-naming
	./test_brew_timer
	./test_audio_sink
	./test_audio_lifetime
	./test_elf_bounds
	@echo "== MMIO de video: leituras chegam aos modelos =="
	ASAN_OPTIONS=detect_leaks=0 ./test_video_mmio
	@echo "== JIT: gancho por instrucao executada =="
	./test_jit_code_hook
	./test_jit_fault_report
	./test_jit_lockstep
	./test_jit_cp15
	./test_jit_unaligned_ldr
	./test_jit_cp15_reset
	./test_jit_lost_write
	./test_jit_unmapped_asymmetry
	./test_control_server
	./test_probe_registry
	./test_save_state
	@echo "[check] zeebo_bootinfo_harness compilou (compile-only; NAO valida bytes reais)."
	@echo "[check] Para validacao real-byte da NAND rode: make test-bootinfo-real"

clean:
	rm -f zeebo_harness zeebo_devices_test zeebo_lle_main zeebo_lle_mod_probe \
	      test_save_state test_audio_sink test_audio_lifetime test_elf_bounds test_video_mmio test_jit_code_hook test_jit_fault_report test_jit_lockstep test_jit_cp15 test_jit_unaligned_ldr test_jit_cp15_reset test_jit_lost_write test_jit_unmapped_asymmetry test_control_server test_l4_mmu test_l4_mmu_uc test_l4_kip_trap test_l4_mmu_txn test_l4_thread_table test_exregs_activation test_l4_ipc_msgtag test_l4_ipc_dispatch test_brew_mif test_brew_timer \
	      zeebo_zwheel_harness zeebo_efs2apps zeebo_rex_harness \
	      zeebo_input_harness test_efs2_fs zeebo_partition zeebo_elf zeebo_boot zeebo_bootinfo_harness \
	      zeebo_lle_txn_probe test_probe_registry test_cli_paths test_intr_pc_resume test_mapcontrol_frame_resume test_threadspace_frame_resume test_exregs_frame_resume test_svc_thumb_resume test_exregs_ig_naming *.o
	rm -rf testkit/build
	$(MAKE) -C gpu -f Makefile.gpu clean

.PHONY: all check test test-cli clean gpu test-gpu test-zwheel test-rex test-core1 test-zwheel-preview test-efs2-zwheel test-efs2-zwheel-loop test-efs2-game-reksio test-debug-agent test-input test-efs2-fs test-bootinfo-real test-txn test-clean-hygiene
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
