# Zeebo LLE debug harness + device models (C++23) — host build
CXX ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra
LIBS := -lunicorn -lcapstone
CPP_HEADERS := $(wildcard *.h gpu/*.h qdsp5/*.h)

all: zeebo_harness zeebo_devices_test zeebo_lle_main zeebo_lle_mod_probe test_save_state test_audio_sink zeebo_bootinfo_harness

# Depend on every directly included subsystem header. The previous short list
# left zeebo_lle_main stale after edits to MMU/control/audio/device headers.
zeebo_lle_main: zeebo_lle_main.cpp $(CPP_HEADERS) qdsp5/qdsp5_dispatcher.cpp qdsp5/audpp_engine.cpp qdsp5/stub_engines.cpp gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp gpu/igl_hook.cpp
	$(CXX) $(CXXFLAGS) -o $@ zeebo_lle_main.cpp qdsp5/qdsp5_dispatcher.cpp qdsp5/audpp_engine.cpp qdsp5/stub_engines.cpp gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp gpu/igl_hook.cpp -lunicorn $(shell pkg-config --cflags --libs sdl2)

zeebo_lle_mod_probe: zeebo_lle_mod_probe.cpp
	$(CXX) $(CXXFLAGS) -o $@ zeebo_lle_mod_probe.cpp -lunicorn

test_save_state: test_save_state.cpp zeebo_save_state.h
	$(CXX) $(CXXFLAGS) -o $@ test_save_state.cpp -lunicorn

test_audio_sink: test_audio_sink.cpp zeebo_audio_sink.h
	$(CXX) $(CXXFLAGS) -o $@ test_audio_sink.cpp

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

# Integração de aliasing via uc_mem_map_ptr (requer libunicorn).
test_l4_mmu_uc: test_l4_mmu_uc.cpp zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -DZEEBO_L4_MMU_WITH_UNICORN -o $@ test_l4_mmu_uc.cpp -lunicorn
	./test_l4_mmu_uc

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
	$(MAKE) -C gpu -f Makefile.gpu gpu_smoke igl_smoke gpu_display_integration igl_transform_smoke igl_guest_bridge_test

test-gpu: gpu
	cd gpu && ./gpu_smoke && ./igl_smoke && ./gpu_display_integration && ./igl_transform_smoke && ./igl_guest_bridge_test

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

test-debug-agent: zeebo_lle_main
	python3 test_zeebo_debug_agent.py

test_control_server: test_control_server.cpp zeebo_control_server.h
	$(CXX) $(CXXFLAGS) -o $@ test_control_server.cpp -pthread

test_probe_registry: test_probe_registry.cpp zeebo_probe_registry.h
	$(CXX) $(CXXFLAGS) -o $@ test_probe_registry.cpp -pthread

test-cli: zeebo_lle_main
	./zeebo_lle_main --cycles=abc >/dev/null 2>&1; test $$? -eq 2
	./zeebo_lle_main --control-port=70000 >/dev/null 2>&1; test $$? -eq 2

# NOTA: `check` inclui APENAS o build compile-only do harness de BootInfo
# (zeebo_bootinfo_harness), que NÃO executa nem afirma validação por bytes
# reais. O gate real que exige a NAND proprietária é `test-bootinfo-real` e
# fica FORA do agregado de propósito, para que um clone limpo (sem NAND) rode
# `check` verde sem jamais alegar validação real-byte que não ocorreu.
check: test test-zwheel-preview test-rex test-zwheel test-efs2apps test-gpu test-input \
       test-efs2-fs test-efs2-zwheel test-efs2-zwheel-loop test-debug-agent test-cli \
       test_l4_mmu test_l4_mmu_uc test_l4_mmu_txn zeebo_bootinfo_harness test_audio_sink test_control_server test_save_state \
       test_probe_registry
	./test_audio_sink
	./test_control_server
	./test_probe_registry
	./test_save_state
	@echo "[check] zeebo_bootinfo_harness compilou (compile-only; NAO valida bytes reais)."
	@echo "[check] Para validacao real-byte da NAND rode: make test-bootinfo-real"

clean:
	rm -f zeebo_harness zeebo_devices_test zeebo_lle_main zeebo_lle_mod_probe \
	      test_save_state test_audio_sink test_control_server test_l4_mmu test_l4_mmu_uc test_l4_mmu_txn \
	      zeebo_zwheel_harness zeebo_efs2apps zeebo_rex_harness \
	      zeebo_input_harness test_efs2_fs zeebo_partition zeebo_elf zeebo_boot zeebo_bootinfo_harness
	$(MAKE) -C gpu -f Makefile.gpu clean

.PHONY: all check test test-cli clean gpu test-gpu test-zwheel test-rex test-core1 test-zwheel-preview test-efs2-zwheel test-efs2-zwheel-loop test-debug-agent test-input test-efs2-fs test-bootinfo-real
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
