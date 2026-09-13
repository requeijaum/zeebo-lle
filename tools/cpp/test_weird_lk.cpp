// test_weird_lk.cpp -- Little Kernel (lk) no modelo REAL do MSM7x00, no harness C++.
//
// Alvo: project/surf-msm7k do lk historico (rev 500c9450, a ultima com suporte MSM7x00), que
// declara ARM_CPU := arm1136j-s -- o mesmo core que o modelo emula.
//
// ORACULO (fatia 1): o lk boota e IMPRIME O BANNER na UART do Zeebo. Aceite medido por este
// harness, nao por nota.
//
// MODELO (o que o guest toca, medido com a sonda em Python antes de virar codigo aqui):
//   * RAM baixa 0x0..16 MB (o layout da SURF: MEMBASE=0/MEMSIZE=8MB; no ARMv6 nao ha VBAR, entao
//     imagem e vetores tem de coincidir);
//   * as TRES UARTs do MSM7x00 (iomap.h do lk: UART1 0xA9A00000, UART2 0xA9B00000, UART3 0xA9C00000).
//     O console do Zeebo e' a UART1 (research/10, board-zeebo.c) -- e o port do lk foi mudado para
//     usa-la, senao ele boota mudo;
//   * janela do proc_comm (0x01F00000), o RPC APPS<->modem. Protocolo conferido no fonte do lk
//     (platform/msm_shared/proc_comm.c):
//        +0x00 APP_COMMAND  +0x04 APP_STATUS  +0x08 APP_DATA1  +0x0C APP_DATA2
//        +0x10 MDM_COMMAND  +0x14 MDM_STATUS  +0x18 MDM_DATA1  +0x1C MDM_DATA2
//        enum: 0=INVALID, 1=READY, 2=CMD_RUNNING, 3=CMD_SUCCESS, 4=CMD_FAIL
//     O lk polla MDM_STATUS (+0x14) ate' valer PCOM_READY(1) e aceita o comando quando APP_STATUS
//     vale PCOM_CMD_SUCCESS(3). E' STUB: no hardware quem responde e' o modem (ARM9).
//
// CONTROLE NEGATIVO: ZEEBO_LK_NOPROC=1 desliga o stub do proc_comm. Sem ele o lk fica preso no
// poll e NAO imprime nada -- se o gate continuar verde com o stub desligado, o gate esta' mentindo.
//
// Uso:  ZEEBO_LK_KERNEL=<lk.bin> ./test_weird_lk     # 0 = passou, 1 = RED, 77 = SKIP
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unicorn/unicorn.h>

#include "zeebo_msm_soc.h"

namespace {

constexpr u64 kLoad = 0x00000000ull;              // imagem e vetores em 0x0 (layout da SURF)
constexpr u32 kLowSize = 16u * 1024u * 1024u;     // RAM baixa modelada (SURF: 8 MB; folga)

// UARTs do MSM7x00, nomes do iomap.h do proprio lk
constexpr u32 kUartBases[3] = {0xa9a00000u, 0xa9b00000u, 0xa9c00000u};
constexpr u32 kUart1 = 0xa9a00000u;               // console do Zeebo (board-zeebo.c)
constexpr u32 kProcComm = 0x01f00000u;
constexpr u32 kVicBase = 0xc0000000u;
constexpr u32 kGptBase = 0xc0100000u;

// registradores da UART do MSM (iguais ao resto do modelo)
constexpr u32 kUartSr = 0x08u, kUartTf = 0x0cu;

std::vector<u8> read_file(const std::string& p) {
    std::vector<u8> out;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        u8 buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
        std::fclose(f);
    }
    return out;
}

struct Estado {
    std::string console;
    u32 console_bytes_por_uart[3] = {0, 0, 0};
    u32 mmio_reads = 0, mmio_writes = 0;
    u32 unmapped[8][3] = {{0}};      // tipo, addr, pc
    u32 n_unmapped = 0;
    bool proc_comm_stub = true;
    u64 key_insn = 0;                // instrucao em que apareceu a primeira chave de console
};

int uart_index(u32 addr) {
    for (int i = 0; i < 3; ++i)
        if (addr >= kUartBases[i] && addr < kUartBases[i] + 0x1000u) return i;
    return -1;
}

void on_mem(uc_engine* uc, uc_mem_type type, u64 addr, int size, i64 value, void* ud) {
    (void)size;
    Estado* st = static_cast<Estado*>(ud);
    const u32 a = static_cast<u32>(addr);
    const bool write = (type == UC_MEM_WRITE);

    const int ui = uart_index(a);
    if (ui >= 0) {
        write ? ++st->mmio_writes : ++st->mmio_reads;
        if (write) {
            if ((a - kUartBases[ui]) == kUartTf) {
                st->console.push_back(static_cast<char>(value & 0xff));
                ++st->console_bytes_por_uart[ui];
            }
        } else {
            // SR: TX_READY|TX_EMPTY (o driver do lk espera isso antes de escrever)
            const u32 v = ((a - kUartBases[ui]) == kUartSr) ? 0x14u : 0u;
            uc_mem_write(uc, a, &v, 4);
        }
        return;
    }
    if (a >= kProcComm && a < kProcComm + 0x1000u) {
        write ? ++st->mmio_writes : ++st->mmio_reads;
        if (!write) {
            const u32 off = a - kProcComm;
            u32 v = 0;
            if (st->proc_comm_stub) {
                if (off == 0x14u)      v = 1u;   // MDM_STATUS  = PCOM_READY
                else if (off == 0x04u) v = 3u;   // APP_STATUS  = PCOM_CMD_SUCCESS (4 = FAIL!)
                else if (off == 0x00u) v = 1u;   // APP_COMMAND: le de volta "aceito"
            }
            uc_mem_write(uc, a, &v, 4);
        }
        return;
    }
    if ((a >= kVicBase && a < kVicBase + 0x1000u) || (a >= kGptBase && a < kGptBase + 0x1000u)) {
        write ? ++st->mmio_writes : ++st->mmio_reads;
        if (!write) { const u32 v = 0; uc_mem_write(uc, a, &v, 4); }
    }
}

void on_unmapped(uc_engine* uc, uc_mem_type type, u64 addr, int size, i64 value, void* ud) {
    (void)size; (void)value;
    Estado* st = static_cast<Estado*>(ud);
    if (st->n_unmapped < 8u) {
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        st->unmapped[st->n_unmapped][0] = static_cast<u32>(type);
        st->unmapped[st->n_unmapped][1] = static_cast<u32>(addr);
        st->unmapped[st->n_unmapped][2] = pc;
        ++st->n_unmapped;
    }
}

}  // namespace

int main() {
    const char* envp = std::getenv("ZEEBO_LK_KERNEL");
    const std::string path = envp ? envp : "";
    if (path.empty()) {
        std::printf("SKIP: defina ZEEBO_LK_KERNEL=<lk.bin> (imagem de SO nao entra no versionamento)\n");
        return 77;
    }
    Estado st;
    st.proc_comm_stub = std::getenv("ZEEBO_LK_NOPROC") == nullptr;

    const std::vector<u8> img = read_file(path);
    if (img.empty()) {
        std::printf("SKIP: imagem ausente/ilegivel: %s\n", path.c_str());
        return 77;
    }
    std::printf("=== lk (surf-msm7k) no modelo REAL do MSM7x00 ===\n");
    std::printf("[lk] imagem: %s (%zu bytes) carregada em 0x%08llx; stub proc_comm=%s\n",
                path.c_str(), img.size(), (unsigned long long)kLoad,
                st.proc_comm_stub ? "ON" : "OFF (controle negativo)");

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        std::printf("FALHOU: uc_open\n");
        return 1;
    }
    uc_ctl_tlb_mode(uc, UC_TLB_CPU);   // modo correto (ver CAUSA-RAIZ-CONGELAMENTO-XV6.md)

    uc_mem_map(uc, 0x00000000u, kLowSize, UC_PROT_ALL);
    uc_mem_write(uc, kLoad, img.data(), img.size());
    for (u32 b : kUartBases) uc_mem_map(uc, b, 0x1000u, UC_PROT_ALL);
    uc_mem_map(uc, kProcComm, 0x1000u, UC_PROT_ALL);
    uc_mem_map(uc, kVicBase, 0x1000u, UC_PROT_ALL);
    uc_mem_map(uc, kGptBase, 0x1000u, UC_PROT_ALL);

    uc_hook h1, h2;
    uc_hook_add(uc, &h1, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void*)on_mem, &st, 1, 0);
    uc_hook_add(uc, &h2, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                         UC_HOOK_MEM_FETCH_UNMAPPED, (void*)on_unmapped, &st, 1, 0);

    // Estado inicial: pilha no topo da RAM baixa (o _start do lk nao depende disso, mas o C sim)
    u32 sp = kLowSize - 0x1000u, pc_entry = static_cast<u32>(kLoad);
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    // ARMADILHA (custou 3 medidas no laco de fatias do xv6 e 1 na sonda): o PC TEM de ser escrito.
    uc_reg_write(uc, UC_ARM_REG_PC, &pc_entry);

    uc_err e = UC_ERR_OK;
    const unsigned long long kFatia = 2000000ull;
    const int kMaxFatias = 40;
    int fatias = 0;
    u32 pc_ant = 0, preso = 0;
    for (;;) {
        u32 pc_now = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc_now);
        e = uc_emu_start(uc, static_cast<u64>(pc_now), 0xFFFFFFFFull, 0, kFatia);
        if (e != UC_ERR_OK) break;
        ++fatias;
        if (fatias >= kMaxFatias) break;
        // Sem progresso de PC = parou. Usar o PC (e nao a contagem) e' o medidor honesto.
        if (pc_now == pc_ant) { if (++preso >= 3u) break; } else { pc_ant = pc_now; preso = 0; }
    }

    u32 pc_fim = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc_fim);
    std::printf("[lk] parou: %s (%d) em %d fatias; pc=0x%08x\n", uc_strerror(e), (int)e, fatias, pc_fim);
    std::printf("[lk] MMIO: %u leituras, %u escritas\n", st.mmio_reads, st.mmio_writes);
    for (u32 i = 0; i < st.n_unmapped; ++i)
        std::printf("[lk] NAO MAPEADO: tipo=%u addr=0x%08x pc=0x%08x\n",
                    st.unmapped[i][0], st.unmapped[i][1], st.unmapped[i][2]);
    std::printf("[lk] console por UART: UART1=0x%08x %u bytes | UART2 %u | UART3 %u\n",
                kUart1, st.console_bytes_por_uart[0], st.console_bytes_por_uart[1],
                st.console_bytes_por_uart[2]);
    std::printf("\n---- console do guest (UART do Zeebo) ----\n%s\n---- fim ----\n",
                st.console.c_str());

    // ORACULO: o banner tem de estar la', e na UART1 (o console do Zeebo).
    const bool tem_banner = st.console.find("welcome to lk") != std::string::npos;
    const bool na_uart1 = st.console_bytes_por_uart[0] > 0u;
    std::printf("\n  %s   banner do lk (\"welcome to lk\")\n", tem_banner ? "OK  " : "FALTA");
    std::printf("  %s   console sai pela UART1 do Zeebo (0x%08x)\n", na_uart1 ? "OK  " : "FALTA", kUart1);
    if (tem_banner && na_uart1) {
        std::printf("=== PASS: o lk boota no modelo real e imprime o banner no console do Zeebo ===\n");
        uc_close(uc);
        return 0;
    }
    std::printf("=== FAIL: o lk nao chegou ao banner (ver pc/NAO MAPEADO acima) ===\n");
    uc_close(uc);
    return 1;
}
