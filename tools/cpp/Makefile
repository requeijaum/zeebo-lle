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

clean:
	rm -f zeebo_harness zeebo_devices_test

.PHONY: all test clean gpu test-gpu
zeebo_partition: zeebo_partition.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_partition.cpp -lunicorn

zeebo_elf: zeebo_elf.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_elf.cpp -lunicorn -lcapstone

zeebo_boot: zeebo_boot.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_boot.cpp -lunicorn
