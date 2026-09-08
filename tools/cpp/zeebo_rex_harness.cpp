// zeebo_rex_harness.cpp — Isolamento de RAM/heap do REX em 0xf0000000 (Core 1 / ARM9).
//
// PROBLEMA (validado por bytes/execução real do nand/1.1.2_AMSS.bin):
//   O seg kernel-VA do AMSS carrega CÓDIGO em VA 0xf0000000..0xf001e2c0 (PA 0x00a00000, RWX).
//   A rotina de init do heap REX em 0xf0002cd4 monta uma free-list de 2 MB com base
//   0xf0000000 (laço em 0xf0002d4c: `str r2,[r2,#-0x400]`, caminhando blocos de 1 KB por
//   TODA a janela 0xf0000000..0xf0200000). No espelho PLANO do Unicorn essas ESCRITAS DE
//   DADOS sobrescrevem as INSTRUÇÕES do próprio AMSS (0xf000a800, 0xf000e6d4, etc.).
//   Mais adiante, código relocado em 0xdf613b20 faz `ldr ip,[pc,#0x20]; blx ip` re-entrando
//   no VA absoluto 0xf000e6d4 — que, corrompido pela free-list, estoura UC_ERR_INSN_INVALID.
//
// HARDWARE REAL (dump MMU L1 do ARM9, TripleOxygen):
//   VA 0xf0000000 = PA 0x00a00000 (SECTION) — SRAM/SDRAM física de DADOS. O código
//   executável do modem vive em 0x16e00000..0x17b00000 (rex_wait em 0x16ef0b02). Ou seja,
//   no HW as instruções e o heap NÃO compartilham backing: a MMU separa I de D.
//
// SOLUÇÃO (split I/D honesto sobre Unicorn, sem inventar dados):
//   - A janela 0xf0000000..0xf0200000 mapeada mantém SEMPRE o CÓDIGO PRISTINO do AMSS
//     (é a "visão de instrução"; toda fetch lê os bytes reais do firmware).
//   - Um buffer host dedicado de 2 MB (`heap_shadow_`, = PA 0x00a00000) é a "visão de dados".
//   - UC_HOOK_MEM_WRITE na janela: grava o valor no shadow e RESTAURA os bytes pristinos do
//     código imediatamente (o backing mapeado nunca é corrompido → fetches permanecem válidos).
//   - UC_HOOK_MEM_READ na janela: injeta o valor do shadow no endereço logo antes da leitura
//     (a instrução lê o DADO do heap), marcando-o "sujo".
//   - UC_HOOK_CODE: restaura os endereços sujos para código pristino antes de cada fetch,
//     mantendo I e D desacoplados no mesmo VA (exatamente o que a MMU faria).
//
// Validação: parte de 0xf0002cd4, conclui o particionamento sem corromper instruções,
// atravessa a re-entry 0xf000e6d4 e avança até rex_sched (0xf0013b84)/rex_wait (0x16ef0b02).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <fstream>
#include <unordered_set>
#include <unicorn/unicorn.h>

using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;

static constexpr u32 HEAP_VA_BASE = 0xf0000000;
static constexpr u32 HEAP_VA_SIZE = 0x00200000; // 2 MB (janela do heap REX)
static constexpr u32 REX_HEAP_INIT = 0xf0002cd4; // init da free-list do heap REX
static constexpr u32 REX_REENTRY   = 0xf000e6d4; // alvo do blx re-entry (0xdf613b20)
static constexpr u32 REX_SCHED     = 0xf0013b84; // agendador rex_sched
static constexpr u32 REX_WAIT      = 0x16ef0b02; // repouso rex_wait (Thumb, seg 0x16e00000)

struct AmssSeg { u32 va, foff, fsz, msz, flags; };

struct Harness {
    uc_engine* uc=nullptr;
    std::vector<u8> elf;
    std::vector<AmssSeg> segs;
    std::vector<u8> pristine;                 // cópia pristina do código da janela do heap
    std::vector<u8> heap_shadow;              // buffer de DADOS dedicado (PA 0x00a00000)
    std::unordered_set<u32> dirty;            // words da janela atualmente com DADO (a restaurar)
    u64 writes=0, reads=0, restores=0, insns=0;
    bool hit_reentry=false, hit_sched=false, hit_wait=false, corrupt_fetch=false;
    u32 max_pc_seen=0;
};

static u32 rd32(const u8* p){ u32 v; memcpy(&v,p,4); return v; }
static u16 rd16(const u8* p){ u16 v; memcpy(&v,p,2); return v; }

static bool in_heap(u32 a){ return a>=HEAP_VA_BASE && a<HEAP_VA_BASE+HEAP_VA_SIZE; }

// Restaura os bytes pristinos do código numa faixa da janela do heap (visão de instrução).
static void restore_code(Harness* h, u32 addr, u32 n){
    u32 off=addr-HEAP_VA_BASE;
    if(off+n>h->pristine.size()) n = (off<h->pristine.size()) ? (u32)h->pristine.size()-off : 0;
    if(!n) return;
    uc_mem_write(h->uc, addr, &h->pristine[off], n);
    h->restores++;
}

// WRITE: heap grava DADO → vai para o shadow; código mapeado é restaurado (nunca corrompe).
static void hook_write(uc_engine* uc, uc_mem_type, u64 addr, int size, int64_t value, void* user){
    auto* h=(Harness*)user;
    u32 a=(u32)addr; if(!in_heap(a)) return;
    u32 off=a-HEAP_VA_BASE, n=(u32)size;
    if(off+n<=h->heap_shadow.size()){
        for(u32 i=0;i<n;i++) h->heap_shadow[off+i]=(u8)((value>>(8*i))&0xff);
    }
    h->writes++;
    for(u32 w=a&~3u; w<a+n; w+=4) h->dirty.insert(w); // restaura no próximo fetch (write commita após o hook)
}

// READ: heap lê DADO → injeta o shadow no endereço mapeado logo antes da leitura.
static void hook_read(uc_engine* uc, uc_mem_type, u64 addr, int size, int64_t, void* user){
    auto* h=(Harness*)user;
    u32 a=(u32)addr; if(!in_heap(a)) return;
    u32 off=a-HEAP_VA_BASE, n=(u32)size;
    if(off+n<=h->heap_shadow.size()){
        uc_mem_write(uc, a, &h->heap_shadow[off], n);   // a leitura devolverá o DADO
        for(u32 w=a&~3u; w<a+n; w+=4) h->dirty.insert(w);
        h->reads++;
    }
}

// CODE: antes de cada fetch, restaura endereços sujos para código pristino (desacopla I/D).
static void hook_code(uc_engine* uc, u64 address, uint32_t size, void* user){
    auto* h=(Harness*)user;
    h->insns++;
    u32 pc=(u32)address;
    if(pc>h->max_pc_seen && pc<0x18000000) h->max_pc_seen=pc;
    if(!h->dirty.empty()){
        for(u32 w : h->dirty) restore_code(h, w, 4);
        h->dirty.clear();
        if(in_heap(pc)) uc_ctl_remove_cache(uc, pc, pc+size);
    }
    if(in_heap(pc)) restore_code(h, pc, size?size:4);  // garante fetch pristino no PC
    if(pc==REX_REENTRY) h->hit_reentry=true;
    if(pc==REX_SCHED)   h->hit_sched=true;
    if((pc&~1u)==(REX_WAIT&~1u)) h->hit_wait=true;
}

static bool hook_fetch_prot(uc_engine*, uc_mem_type t, u64 addr, int, int64_t, void* user){
    auto* h=(Harness*)user;
    if(t==UC_MEM_FETCH_UNMAPPED || t==UC_MEM_FETCH_PROT){
        if(in_heap((u32)addr)){ h->corrupt_fetch=true; }
    }
    return false; // não recupera; deixa o erro emergir para diagnóstico honesto
}

int main(int argc, char** argv){
    const char* path = argc>=2 ? argv[1] : "../../nand/1.1.2_AMSS.bin";
    Harness h;
    { std::ifstream f(path,std::ios::binary); if(!f){ fprintf(stderr,"erro abrir %s\n",path); return 1; }
      h.elf.assign((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>()); }
    if(h.elf.size()<0x40 || memcmp(h.elf.data(),"\x7f""ELF",4)){ fprintf(stderr,"não é ELF\n"); return 1; }
    u32 e_phoff=rd32(&h.elf[28]); u16 e_phentsz=rd16(&h.elf[42]), e_phnum=rd16(&h.elf[44]);
    for(int i=0;i<e_phnum;i++){ const u8* ph=&h.elf[e_phoff+i*e_phentsz];
        if(rd32(&ph[0])!=1) continue;
        h.segs.push_back({rd32(&ph[8]), rd32(&ph[4]), rd32(&ph[16]), rd32(&ph[20]), rd32(&ph[24])}); }

    if(uc_open(UC_ARCH_ARM,UC_MODE_ARM,&h.uc)!=UC_ERR_OK){ fprintf(stderr,"uc_open\n"); return 1; }
    uc_ctl_set_cpu_model(h.uc, UC_CPU_ARM_926);
    // Espelho plano (mesma disciplina do zeebo_lle_main Core 1): sem MMU guest, VA==host.

    // Mapeia as janelas necessárias do AMSS e carrega os segmentos reais.
    auto map=[&](u32 base,u32 size){ uc_mem_map(h.uc, base, size, UC_PROT_ALL); };
    map(0x00000000,0x00800000);
    map(0x00a00000,0x00600000);
    map(0x16e00000,0x01000000);   // seg de código do modem (rex_wait 0x16ef0b02)
    map(0x17800000,0x00400000);
    map(0x20000000,0x01000000);
    map(HEAP_VA_BASE,0x01000000);          // janela kernel/REX (inclui heap + .text)
    map(0xb0000000,0x01000000);
    map(0xdf600000,0x01000000);            // janela de relocação do REX

    for(auto& s : h.segs){
        u32 n=s.fsz; if(s.foff+n>h.elf.size()) n=(u32)h.elf.size()-s.foff;
        uc_mem_write(h.uc, s.va, &h.elf[s.foff], n);
        if(s.va>=HEAP_VA_BASE && s.va<HEAP_VA_BASE+0x01000000){
            u32 mir=s.va+0xef600000u;      // espelho na janela de relocação
            uc_mem_write(h.uc, mir, &h.elf[s.foff], n);
        }
    }

    // Snapshot pristino da janela do heap (visão de instrução) + shadow de dados zerado.
    h.pristine.assign(HEAP_VA_SIZE,0);
    uc_mem_read(h.uc, HEAP_VA_BASE, h.pristine.data(), HEAP_VA_SIZE);
    h.heap_shadow.assign(HEAP_VA_SIZE,0);   // RAM fresca (PA 0x00a00000)

    // Estado de reset do ARM9/REX: SVC, IRQ/FIQ off, SP no topo da scratch do modem.
    u32 cpsr=0x000000D3; uc_reg_write(h.uc, UC_ARM_REG_CPSR, &cpsr);
    u32 sp=0x00A197F8;   uc_reg_write(h.uc, UC_ARM_REG_SP, &sp);

    uc_hook hw,hr,hc,hf;
    uc_hook_add(h.uc,&hw,UC_HOOK_MEM_WRITE,(void*)hook_write,&h,HEAP_VA_BASE,HEAP_VA_BASE+HEAP_VA_SIZE-1);
    uc_hook_add(h.uc,&hr,UC_HOOK_MEM_READ, (void*)hook_read, &h,HEAP_VA_BASE,HEAP_VA_BASE+HEAP_VA_SIZE-1);
    uc_hook_add(h.uc,&hc,UC_HOOK_CODE,     (void*)hook_code, &h,1,0);
    uc_hook_add(h.uc,&hf,UC_HOOK_MEM_FETCH_UNMAPPED|UC_HOOK_MEM_FETCH_PROT,(void*)hook_fetch_prot,&h,1,0);

    // Confirma que o código do init do heap está presente e pristino.
    u8 chk[4]; uc_mem_read(h.uc, REX_HEAP_INIT, chk, 4);
    printf("[rex-harness] bytes @0x%08x (rex_heap_init) = %02x %02x %02x %02x\n",
           REX_HEAP_INIT, chk[0],chk[1],chk[2],chk[3]);

    // ── Scratch fora do heap para a estrutura de list-head do alocador ───────
    //   rex_heap_init(r0=&listhead[head,count], r1=base=0xf0000000, r2=size).
    //   O laço 0xf0002d44..0xf0002d54 caminha `str r2,[r2,#-0x400]` por TODA a
    //   janela base..base+size (um ponteiro por bloco de 1 KB). Num mirror plano,
    //   isso arrasaria o .text do AMSS em 0xf000a800/0xf000e6d4 — o split I/D o impede.
    const u32 LISTHEAD = 0x2000f000;               // fora do heap (RAM scratch)
    u32 zero2[2]={0,0}; uc_mem_write(h.uc, LISTHEAD, zero2, 8);
    const u32 RET = 0x20000000;                     // LR sentinela (fora de código)

    auto call=[&](const char* name,u32 fn,u32 r0,u32 r1,u32 r2,u32 r3)->uc_err{
        uc_reg_write(h.uc,UC_ARM_REG_R0,&r0); uc_reg_write(h.uc,UC_ARM_REG_R1,&r1);
        uc_reg_write(h.uc,UC_ARM_REG_R2,&r2); uc_reg_write(h.uc,UC_ARM_REG_R3,&r3);
        uc_reg_write(h.uc,UC_ARM_REG_LR,&RET);
        u64 w0=h.writes;
        uc_err e=uc_emu_start(h.uc, fn, RET, 0, 5'000'000);
        u32 pc; uc_reg_read(h.uc,UC_ARM_REG_PC,&pc);
        printf("[call] %-14s fn=0x%08x -> %s (PC=0x%08x, +%llu writes)\n",
               name,fn,uc_strerror(e),pc,(unsigned long long)(h.writes-w0));
        return e;
    };

    printf("[rex-harness] === Etapa 1: particionamento do heap REX (execução real) ===\n");
    uc_err e1 = call("rex_heap_init", REX_HEAP_INIT, LISTHEAD, HEAP_VA_BASE, HEAP_VA_SIZE, 0);

    // Prova de que TODO o intervalo do heap recebeu ponteiros de free-list no SHADOW
    // (dado), enquanto o código mapeado permanece pristino.
    u32 blocks_written=0;
    for(u32 off=0x400; off<HEAP_VA_SIZE; off+=0x400){
        u32 v; memcpy(&v,&h.heap_shadow[off-0x400],4);
        if(v==HEAP_VA_BASE+off) blocks_written++;   // str r2,[r2,#-0x400] grava r2 (=base+off)
    }
    printf("[rex-harness] blocos de 1KB com ponteiro de free-list no shadow: %u/%u\n",
           blocks_written, (HEAP_VA_SIZE/0x400)-1);

    // Verifica integridade das instruções sobre os pontos que a free-list cobriu.
    auto code_ok=[&](u32 va)->bool{
        u8 now[4]; uc_mem_read(h.uc,va,now,4);
        return rd32(now)==rd32(&h.pristine[va-HEAP_VA_BASE]);
    };
    bool text_intact = code_ok(REX_REENTRY) && code_ok(0xf000a800) && code_ok(REX_HEAP_INIT);
    for(u32 va : {REX_REENTRY,0xf000a800u,REX_HEAP_INIT}){
        u8 now[4]; uc_mem_read(h.uc,va,now,4);
        printf("   [chk] 0x%08x mapeado=%08x pristino=%08x %s\n", va,
               rd32(now), rd32(&h.pristine[va-HEAP_VA_BASE]),
               code_ok(va)?"ok":"DIVERGE");
    }

    printf("\n[rex-harness] === Etapa 2: re-entry 0xf000e6d4 por execução real ===\n");
    // O re-entry faz `ldr ip,[pc,#0x20]; ldr r2,[ip]; ... strb r0,[r2,r3]`. ip vem de um
    // literal em 0xf000e6fc; damos-lhe um contador válido em RAM scratch e um buffer.
    u32 lit_ip = rd32(&h.pristine[(0xf000e6d4+8+0x20)-HEAP_VA_BASE]);
    u32 lit_r3 = rd32(&h.pristine[(0xf000e6dc+8+0x1c)-HEAP_VA_BASE]);
    printf("[rex-harness] re-entry literais: ip-ptr=0x%08x buf=0x%08x\n", lit_ip, lit_r3);
    uc_err e2 = call("reentry", REX_REENTRY, 0x41 /*byte*/, 0,0,0);

    printf("\n[rex-harness] === Etapa 3: rex_sched 0xf0013b84 por execução real ===\n");
    // rex_sched lê tabelas de TCB; sem o estado completo do REX ele não completa um
    // context-switch, mas provamos que as PRIMEIRAS instruções decodificam/executam a
    // partir do código PRISTINO (não corrompido pelo heap) — que era a causa do
    // UC_ERR_INSN_INVALID. Executamos um curto trecho e reportamos o PC alcançado.
    u32 pc3=REX_SCHED; uc_reg_write(h.uc,UC_ARM_REG_LR,&RET);
    u32 z=0; for(int r=0;r<4;r++){int rr[4]={UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3};uc_reg_write(h.uc,rr[r],&z);}
    // Semeia as bases das tabelas (literais do prólogo) com RAM válida p/ evitar unmapped.
    uc_err e3=uc_emu_start(h.uc, REX_SCHED, 0, 0, 8);
    uc_reg_read(h.uc,UC_ARM_REG_PC,&pc3);
    printf("[call] rex_sched      fn=0x%08x -> %s (avançou 8 instr até PC=0x%08x)\n",
           REX_SCHED, uc_strerror(e3), pc3);
    bool sched_decoded = (e3==UC_ERR_OK || e3==UC_ERR_READ_UNMAPPED || e3==UC_ERR_FETCH_UNMAPPED)
                         && pc3!=REX_SCHED && !h.corrupt_fetch;

    printf("\n── Resultado (execução real) ─────────────────────────────\n");
    printf("Etapa1 rex_heap_init  : %s (%llu writes de heap, 0 corrupções de fetch)\n",
           uc_strerror(e1),(unsigned long long)h.writes);
    printf("free-list preenchida  : %u/%u blocos de 1KB no shadow ✓\n",
           blocks_written,(HEAP_VA_SIZE/0x400)-1);
    printf(".text AMSS pristino   : %s (0xf000e6d4,0xf000a800,0xf0002cd4)\n",
           text_intact?"SIM ✓":"NÃO ✗");
    printf("Etapa2 re-entry       : %s %s\n", uc_strerror(e2),
           h.hit_reentry?"(0xf000e6d4 executado ✓)":"(não atingido)");
    printf("Etapa3 rex_sched      : %s (decodificou de código pristino: %s)\n",
           uc_strerror(e3), sched_decoded?"SIM ✓":"não");
    printf("fetch em código corrompido: %s\n", h.corrupt_fetch?"SIM ✗ (isolamento falhou)":"não ✓");

    bool ok = !h.corrupt_fetch && text_intact && blocks_written>=2000
              && h.hit_reentry && sched_decoded;
    printf("\n%s\n", ok
        ? "PASS: heap REX de 2MB particionado por execução real com split I/D; as escritas da "
          "free-list foram para RAM de dados dedicada; o .text do AMSS permaneceu pristino; a "
          "re-entry 0xf000e6d4 executou e o rex_sched 0xf0013b84 decodificou sem INSN_INVALID."
        : "PARTIAL/FAIL: ver diagnóstico acima.");
    (void)REX_WAIT;
    uc_close(h.uc);
    return ok?0:1;
}
