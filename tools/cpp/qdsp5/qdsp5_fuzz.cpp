// qdsp5_fuzz.cpp — fuzz dirigido do parser ONCRPC do skeleton QDSP5 (Q0.1 hardening).
//
// Objetivo: quando o hook Q0.1 for plugado ao vivo, o firmware pode entregar
// pacotes malformados (len curto, payload_len absurdo, proc fora do range,
// pcm_bytes gigante/ímpar, ponteiros inválidos). Este harness martela
// Qdsp5Dispatcher::feed_raw() e o parser de qdsp5_capture_hook.h com buffers
// arbitrários para achar OOB read/write, overflow de inteiro e faltas de
// validação — ANTES do live plug.
//
// A "memória guest" aqui é ESTRITAMENTE bounds-checked (um bloco host fixo), de
// modo que qualquer estouro reportado pelo ASAN seja bug de LÓGICA do parser/engine,
// não do harness. Nenhum deref de ponteiro do host arbitrário é possível pelo guest.
//
// Build (libFuzzer, preferido):
//   clang++ -std=c++23 -g -O1 -fsanitize=fuzzer,address \
//     qdsp5_fuzz.cpp qdsp5_dispatcher.cpp audpp_engine.cpp stub_engines.cpp -o qdsp5_fuzz
// Fallback (sem clang): driver próprio com g++ + ASAN (define QDSP5_FUZZ_STANDALONE),
//   itera N milhões de buffers aleatórios+mutados de stdin/semente.
//
// Ver Makefile.qdsp5 target `fuzz` / `fuzz-standalone`.

#include "qdsp5_dispatcher.h"
#include "qdsp5_capture_hook.h"
#include "iqdsp_engine.h"
#include <cstdint>
#include <cstring>
#include <vector>

using namespace zeebo::qdsp5;

// ---- Memória guest de teste: ESTRITA, bounds-checked ----------------------
// Bloco host fixo mapeado num VA base. Qualquer leitura fora de [base,base+N)
// falha (return false). Assim o engine que segue ponteiros do payload NUNCA
// toca memória host arbitrária pelo caminho do guest — os únicos estouros que
// o ASAN pode pegar são de buffers alocados pelo próprio parser/engine.
namespace {
constexpr u32 kGuestBase = 0x02000000;
constexpr u32 kGuestSize = 0x8000; // 32 KiB de "shared RAM" de teste
alignas(16) uint8_t g_guest_mem[kGuestSize];

bool guest_read(u32 va, void* dst, u32 size, void*) {
    if (size == 0) return true;
    if (va < kGuestBase) return false;
    u32 off = va - kGuestBase;
    // guarda contra overflow de off+size e contra passar do bloco
    if (off > kGuestSize) return false;
    if (size > kGuestSize - off) return false;
    std::memcpy(dst, g_guest_mem + off, size);
    return true;
}

QdspGuest make_test_guest() {
    // conteúdo determinístico só p/ ter algo a copiar
    for (u32 i = 0; i < kGuestSize; i++) g_guest_mem[i] = (uint8_t)(i * 131 + 7);
    QdspGuest g; g.read = &guest_read; g.ctx = nullptr;
    return g;
}
} // namespace

// ---- Alvo comum: exercita TODOS os parsers com o mesmo buffer -------------
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Guest recriado barato (memória estática); dispatcher por iteração para
    // exercitar construção/estado limpo dos engines.
    static QdspGuest guest = make_test_guest();

    // len varia: usa o size do fuzzer inteiro, e também um len "declarado"
    // derivado dos bytes para exercitar len < size / len > size.
    // (feed_raw usa `len` como fronteira do buffer; passar len > size seria bug
    //  DO HARNESS, então len é sempre <= size.)
    u32 len = (u32)size;

    // 1) Caminho principal: feed_raw (decodifica header ONCRPC + roteia + engine)
    {
        Qdsp5Dispatcher disp;
        disp.on_completion = [](u32, u32) {};
        (void)disp.feed_raw(data, len, guest, /*caller_tcb=*/0xABCD1234);
    }

    // 1b) feed_raw com lens truncados derivados do input (exercita pacote curto,
    //     len exatamente no header, no +0x24, no +0x80, e além).
    if (size >= 1) {
        u32 probes[] = {0, 1, 4, 0x0c, 0x20, 0x24, 0x40, 0x7f, 0x80, 0x81, 0x100, 0x4ff, 0x500, 0x501};
        for (u32 pl : probes) {
            u32 l = pl <= size ? pl : (u32)size;
            Qdsp5Dispatcher disp;
            (void)disp.feed_raw(data, l, guest, 0);
        }
    }

    // 2) Parser puro do capture hook (Q0.1) — parse_packet + report_capture.
    //    report_capture faz hexdump do payload; alvo p/ OOB de leitura.
    {
        CapturedRpc c = parse_packet(data, size);
        // report_capture imprime; silencie via engano barato: só o parse é
        // suficiente p/ o ASAN pegar OOB, mas chamamos report também com dump
        // pequeno p/ cobrir o loop de hexdump (que lê pkt[OFF_PAYLOAD+i]).
        report_capture(c, data, size, /*dump=*/16);
    }

    // 3) Engine AUDPP direta: trata o buffer inteiro como payload decodificado.
    //    Alvo do bug de pcm_bytes (aloca (pcm_bytes/2) int16 e lê pcm_bytes).
    {
        auto eng = make_audpp_engine();
        std::vector<u8> body(data, data + size);
        Command cmd{Engine::Audpp, prog::AUDMGR, 0, std::move(body)};
        (void)eng->handle(cmd, guest);
    }

    return 0;
}

// ---- Driver standalone (fallback g++ sem clang/libFuzzer) ------------------
#ifdef QDSP5_FUZZ_STANDALONE
#include <cstdio>
#include <cstdlib>
#include <random>

int main(int argc, char** argv) {
    // Nº de iterações: argv[1] ou 3M por padrão.
    uint64_t iters = (argc > 1) ? strtoull(argv[1], nullptr, 0) : 3000000ULL;
    uint64_t seed  = (argc > 2) ? strtoull(argv[2], nullptr, 0) : 0xC0FFEE;

    std::mt19937_64 rng(seed);
    std::vector<uint8_t> buf;

    // Sementes estruturadas: pacote AUDMGR válido-ish e variações.
    auto seed_pkt = []() {
        std::vector<uint8_t> p(0x500, 0);
        auto w32 = [&](size_t off, uint32_t v){ std::memcpy(p.data()+off, &v, 4); };
        w32(0x0c, 0x30000013); // program AUDMGR
        w32(0x10, 1);          // version
        w32(0x20, 9);          // procedure
        w32(0x24, 0x18);       // payload_len
        w32(0x80, 0x01);       // opcode PLAY
        w32(0x84, 0x02000000); // pcm_ptr
        w32(0x88, 0x1000);     // pcm_bytes
        w32(0x8c, 44100);      // sample_rate
        w32(0x90, 2);          // channels
        w32(0x94, 65536);      // vol
        return p;
    };
    std::vector<std::vector<uint8_t>> corpus = { seed_pkt(), {}, {0}, std::vector<uint8_t>(0x80,0xff) };

    for (uint64_t it = 0; it < iters; it++) {
        // 50% mutar semente, 50% buffer aleatório de tamanho variável.
        if ((rng() & 1) && !corpus.empty()) {
            buf = corpus[rng() % corpus.size()];
            // mutações: flips e resize
            int muts = 1 + (rng() % 32);
            for (int m = 0; m < muts && !buf.empty(); m++)
                buf[rng() % buf.size()] ^= (uint8_t)(1u << (rng() % 8));
            if ((rng() % 4) == 0) buf.resize(rng() % 0x520);
        } else {
            size_t n = rng() % 0x520; // até um pouco além de kMaxPayload
            buf.resize(n);
            for (auto& b : buf) b = (uint8_t)rng();
        }
        LLVMFuzzerTestOneInput(buf.data(), buf.size());
        if ((it & 0xFFFFF) == 0)
            fprintf(stderr, "[fuzz] %llu iters...\n", (unsigned long long)it);
    }
    fprintf(stderr, "[fuzz] DONE %llu iters, no crash.\n", (unsigned long long)iters);
    return 0;
}
#endif
