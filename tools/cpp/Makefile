# Zeebo LLE debug harness + device models (C++23) — host build
CXX ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra
LIBS := -lunicorn -lcapstone

all: zeebo_harness zeebo_devices_test zeebo_lle_main zeebo_lle_mod_probe test_save_state test_audio_sink

zeebo_lle_main: zeebo_lle_main.cpp zeebo_brew_loader.h gpu/igl_guest_bridge.h qdsp5/qdsp5_dispatcher.cpp qdsp5/audpp_engine.cpp qdsp5/stub_engines.cpp gpu/soft_rasterizer.cpp gpu/rasterizer_factory.cpp gpu/igl_hook.cpp
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

# GPU subsystem (tools/cpp/gpu/) — build isolado + verificação por framebuffer.
gpu:
	$(MAKE) -C gpu -f Makefile.gpu gpu_smoke igl_smoke gpu_display_integration igl_transform_smoke

test-gpu: gpu
	cd gpu && ./gpu_smoke && ./igl_smoke && ./gpu_display_integration && ./igl_transform_smoke

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

# Passo 5: mapeamento Z-Pad/SDL2 → AVK e despacho contínuo de EVT_KEY_PRESS/
# EVT_KEY_RELEASE ao manipulador BREW (HandleEvent), provado por execução real
# sob Unicorn (applet stub que consome a tecla e retorna r0=1).
zeebo_input_harness: zeebo_input_harness.cpp zeebo_brew_loader.h zeebo_l4_mmu.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_input_harness.cpp -lunicorn

test-input: zeebo_input_harness
	./zeebo_input_harness

clean:
	rm -f zeebo_zwheel_harness zeebo_efs2apps zeebo_rex_harness zeebo_input_harness

.PHONY: all test clean gpu test-gpu test-zwheel test-rex test-core1 test-zwheel-preview test-input test-efs2-fs
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
