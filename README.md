# beLLEZeebo

> *"Better to emulate in Hell than serve in Heaven."* 😈🤘  
> **beLLEZeebo** (Belzebub / Beelzebub + **LLE** + **Zeebo**) is an experimental low-level emulator (LLE) for the enigmatic **Zeebo** video game console, running bare-metal Qualcomm MSM7201A dual-core firmware dumps directly on virtual silicon.

---

## The Console: A Forgotten Beast

Launched in Brazil in 2009 by Tectoy and Qualcomm (and later in Mexico and China), the **Zeebo** was intended as a 3G-connected, digital-only home console for developing markets.

Under the hood, it wasn't typical console hardware. It was essentially a carrier smartphone motherboard repackaged into a desktop shell:
- **SoC:** Qualcomm MSM7201A
- **CPUs:** Dual-core ARM architecture:
  - **Core0 (Applications Processor):** ARM1136J-S @ 528 MHz
  - **Core1 (Modem/Baseband Processor):** ARM926EJ-S @ 256 MHz running AMSS/REX RTOS
- **Operating Environment:** OKL4 Microkernel hosting Qualcomm **BREW 4.0.2**
- **GPU:** Qualcomm Adreno 130 (Yamato / AMD Imageon Z430 family, OpenGL ES 1.1)
- **Audio/DSP:** Qualcomm QDSP5
- **Storage & Networking:** 1 GB NAND flash, built-in 3G modem (Z-Net digital distribution)

When Qualcomm and Tectoy shut down the Z-Net servers in 2011, Zeebo consoles were effectively bricked from obtaining new titles, locking their library away and turning preservation into a reverse-engineering nightmare.

---

## The Emulation Landscape: LLE vs. HLE

To understand why **beLLEZeebo** exists, it helps to understand how the Zeebo preservation scene has tackled the system:

| Paradigm | Projects | How It Works | Strengths & Weaknesses |
| :--- | :--- | :--- | :--- |
| **HLE** *(High-Level Emulation)* | **Zeebulator**, **Zeemu**, **Infuse**, **zeebx** | Emulates only the user-space ARM game binary (`.mod`) and intercepts high-level BREW C++ APIs (IShell, IDisplay, IFile, OpenGL ES). | **Fast & portable.** Does not require copyrighted firmware dumps or baseband code. However, it requires manually stubbing and reverse-engineering hundreds of proprietary BREW API calls. Subtle OS timing or undocumented behaviors break easily. |
| **LLE** *(Low-Level Emulation)* | **beLLEZeebo** (`zeebo-lle`) | Emulates the raw MSM7201A silicon: dual-core execution, MMU, VIC/GPT timers, ProcComm/SMD inter-processor channels, NAND boot flow, and OKL4 kernel spaces. | **True-to-hardware fidelity.** Boots original dumped NAND images (AMSS + APPS) verbatim. It uncovers how the machine actually ran, but demands grueling hardware documentation and exact SoC register behaviors. |

---

## Current Architecture & Scope

`beLLEZeebo` orchestrates execution using Unicorn (ARM engine) with custom C++23 SoC peripherals:
- **Dual-Core Orchestration:** Concurrent execution of ARM1136 Core0 (APPS) and ARM926 Core1 (AMSS modem processor).
- **Inter-Processor Comms (IPC):** Hardware mailboxes, ProcComm registers, and shared memory (SMEM) bridges.
- **Microkernel / OS Support:** Handles OKL4 SpaceManager address space switching, thread contexts, and page fault resolution.
- **Hardware Peripherals:** Vectored Interrupt Controller (VIC), General Purpose Timers (GPT), and NAND relocator structures.
- **Peripheral & Audio Stacks:** Ongoing reverse-engineering of the Adreno 130 command stream and QDSP5 RPC protocols.

---

## Booting a real Linux kernel (`linux-boot` branch)

Beyond the firmware path, the project boots a **real Linux kernel (3.4.113 for
MSM/ARMv6)** inside the emulator, with a clean BusyBox initramfs and an interactive
shell. It is the fastest way to exercise the SoC models end to end, and it is what the
peripheral work is validated against.

- Harness: `tools/cpp/test_linux_boot.cpp` (`make -C tools/cpp test_linux_boot`).
- Kernels, kernel patches and rootfs sources: `testdata/kernels/`,
  `testdata/kernel-patches/`, `testdata/rootfs/`.
- Debug window (SDL2/Wayland, 1x2): the left pane is the guest's UART console, the
  right pane is the **guest framebuffer** — the shell runs on the VT console, so what
  you type shows up in the framebuffer pane.
- Validated by execution: clean rootfs + shell, `fb0` at 720x480 RGB565, GPT/DGT
  clockevent, MDP/VIC, the UART consoles, and a working **EHCI host controller** (the
  hub enumerates a high-speed device). HID keyboard enumeration is the open item.

Full details, peripheral map and debugging env vars: [docs/linux-boot.md](docs/linux-boot.md).

---

## Building

### Requirements
- Modern C++23 compiler (`clang++` or `g++`)
- `libunicorn-dev` (Unicorn CPU emulator engine)
- `libsdl2-dev` (for display sink)
- Python 3 (for test and diagnostic tooling)

### Quick Start
```bash
# Build the core test suite and harness
make check-fast

# Compile the standalone LLE boot binary
make tools/cpp/zeebo_lle_main
```

---

## Disclaimer & Legal

`beLLEZeebo` is an independent reverse-engineering research project dedicated to computer history and digital preservation. It contains no proprietary firmware, NAND flash dumps, or game assets. All product names, trademarks, and registered trademarks belong to their respective owners.
