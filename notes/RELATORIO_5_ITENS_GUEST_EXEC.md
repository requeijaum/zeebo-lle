# Relatório: os 5 itens que faltam para executar jogos — investigação + esqueletos

Autor: agente QDSP5 (subsistema isolado). Público: agente da base LLE.
Base factual: leitura do código atual de `tools/cpp/zeebo_lle_main.cpp`, `zeebo_l4_mmu.h`,
`notes/FINDINGS.md` (auditoria 5a), e boots headless (`/tmp/lle_boot_night.log`).
Critério: bytes/execução real, nunca insn-count (padrão de ouro do Rafael). Onde é
inferência, está marcado [infer].

Ordem de prioridade (cada um destrava o próximo):
  1. Core1/AMSS não roda o REX  ← mais estrutural, ataca primeiro
  2. MAP_CONTROL: de "mapeia" para "mapeia o suficiente p/ não derailar"
  3. Core0 loop 0xb000d4a8 (poll de bit de status)
  4. Loader/dispatch de applet BREW (.mod / AEEMod_Load)
  5. Frame loop integrado (GPU real → display, hoje é test pattern)

---

## Estado que MUDOU desde a auditoria 5a (bom — creditar o progresso)

A auditoria 5a (FINDINGS.md:1501) foi dura e correta na época: saltos hardcoded
(`core0_.entry = 0x1013a000` quando `c>=38`) + shim retorno-sucesso lidos como boot.
Verifiquei o código ATUAL e dois desses débitos já foram pagos:

- O salto hardcoded 0x1013a000 SUMIU. `run_interleaved` (linha ~431) avança os dois cores
  só por `uc_emu_start(...)` + releitura de PC. Progressão natural, sem force.
- MAP_CONTROL virou mapeamento REAL: `handle_map_control` em `zeebo_l4_mmu.h` decodifica
  UTCB, lê os MRs (phys_desc/fpage), e chama `map_one()` (uc_mem_map real). Não é mais o
  shim de valor rejeitado em 2g.

Ou seja: a fundação avançou. Os 5 itens abaixo são o que sobra — e agora são gargalos
de EXECUÇÃO real, não de andaime.

---

## ITEM 1 — Core1/AMSS não escalona o REX (crash pc=0x00fffffe)

### Evidência
- `core1_.entry = rd32(d.data(), 24)` = e_entry do ELF = **0x00a00000** (confirmado:
  `python3 struct` no AMSS.bin dá e_entry 0x00a00000).
- No boot, Core1 sobe o PC LINEAR +0x9c40/ciclo (0x00a09c40, 0x00a13880, 0x00a1d4c0...)
  até bater em 0x01000000 (fim do seg13 de código) e cair em UC_ERR_FETCH_UNMAPPED.
- 0x9c40 = 40000 = `slice_insns * 4`. Isso é PROVA de NOP-slide: o core só soma PC, não
  executa branch nenhum. (Mesmo diagnóstico da auditoria 5a item 4, ainda vale.)

### Causa raiz [infer, alta confiança]
0x00a00000 é o e_entry do super-ELF, mas o **primeiro código a rodar no ARM9 não é o
e_entry** — é o reset vector / o AMSS boot shim que instala stack, MMU e pula pro
`rex_init`/`rex_task_start`. Rodar a partir de 0x00a00000 cru cai em bytes que não são
o preâmbulo de reset → slide. Além disso não há setup de:
  - stack pointer do modem (SP do REX)
  - vetores de exceção ARM9 (0xffff0000 ou 0x00000000)
  - MMU/regiões do modem (o AMSS espera seu próprio espaço mapeado)

### O que investigar (evidência a coletar)
1. Achar o REAL entry de reset do AMSS. No QSC/MSM, o modem começa por um shim que a
   auditoria já tocou (handshake loader→kernel 0x20020005, FINDINGS). Buscar no AMSS.bin
   o preâmbulo clássico ARM reset: `msr cpsr_c, #0xd3` (troca p/ SVC, IRQ off) seguido de
   `ldr sp, =...`. Grep por bytes `d3 f0 21 e3` (msr) perto do início dos segmentos.
2. Confirmar se existe um segmento com VA baixo que seja o vetor real (não 0x00a00000).

### Esqueleto — bring-up mínimo do Core1 antes do loop
```cpp
// Em setup do Core1, ANTES do run_interleaved:
// (a) achar o entry de reset real — não confiar cegamente no e_entry.
u32 amss_reset = find_amss_reset_vector(d);   // varre msr cpsr / ldr sp; fallback e_entry
if (amss_reset) core1_.entry = amss_reset;

// (b) preâmbulo que o hardware faria e o ELF cru não faz:
uc_reg_write(core1_.uc, UC_ARM_REG_CPSR, &(u32){0xD3}); // SVC mode, IRQ/FIQ off
u32 sp = 0x00b16000; // topo de RAM do modem [infer: usar 1 seg de scratch conhecido]
uc_reg_write(core1_.uc, UC_ARM_REG_SP, &sp);

// (c) vetores de exceção: mapear página em 0x00000000 (ou 0xffff0000 se VBAR high)
//     senão qualquer IRQ do timer derruba o core.

// (d) GUARD-RAIL de diagnóstico: hook que detecta NOP-slide e PARA cedo, em vez de
//     rodar 160 ciclos até crashar em 0xfffffe. Salva tempo de iteração.
uc_hook h;
uc_hook_add(core1_.uc, &h, UC_HOOK_CODE, (void*)core1_slide_detector,
            &core1_, 0x00a00000, 0x01000000);
// core1_slide_detector: se (pc - last_pc) == 4 por N insns seguidas sem nenhum
// branch tomado -> printf("[Core1] NOP-slide @0x%x, entry provavelmente errado") e
// uc_emu_stop(). Transforma 90s de boot cego em feedback imediato.
```

### Critério de sucesso (byte-level, não insn-count)
Core1 deve executar um BRANCH real (pc não-linear) dentro dos primeiros ~1000 insns e
alcançar um laço de scheduler REX identificável (ex.: rex_wait @0x16ef0b02, que a base já
documentou no commit de 07-09 14:53). Enquanto o pc subir +4 constante, está errado.

---

## ITEM 2 — MAP_CONTROL: de "mapeia" para "mapeia o que o derail precisa"

### Evidência
`handle_map_control` já faz `map_one()` real. Mas a auditoria 5a (item 2) e as notas 2g
apontam o modo de falha clássico: MAP_CONTROL retorna sucesso mas a página mapeada não
contém o código/dados válidos (o loader deveria ter copiado a imagem da task pra lá antes).
Resultado: APPS salta pra RAM mapeada porém VAZIA → derail "1999/1999 NOP-slide".

### O que investigar
1. Instrumentar CADA map_one: logar va/phys/size E fazer um dump dos primeiros 16 bytes
   da página de origem (phys) — se for tudo 0x00/0xFF, o mapeamento é "correto porém
   inútil": falta o loader popular a fonte.
2. Cruzar o va mapeado com o destino do primeiro salto pós-MAP_CONTROL. Se APPS mapeia
   0x0048xxxx e salta pra lá, verificar se a imagem da task foi relocada pra esse phys.

### Esqueleto — validação de conteúdo no map_one
```cpp
uc_err map_one_checked(uc_engine* uc, const MapItem& it) {
    uc_err e = map_one(uc, it);
    if (e != UC_ERR_OK) return e;
    // Ler os primeiros bytes da página FONTE (phys) já mapeada:
    u8 probe[16] = {0};
    uc_mem_read(uc, it.fpage.vaddr(), probe, sizeof probe);
    bool empty = true;
    for (u8 b : probe) if (b != 0x00 && b != 0xFF) { empty = false; break; }
    if (empty) {
        printf("[MAP_CONTROL][WARN] va=0x%llx mapeada porém VAZIA (0x%02x...) "
               "-> o loader não populou a task. Derail provável aqui.\n",
               (unsigned long long)it.fpage.vaddr(), probe[0]);
    }
    return e;
}
```
Se o WARN disparar, o fix não é no MAP_CONTROL — é garantir que a cópia da task (pelo
loader OKL4 ou pelo relocador NAND/DMOV) aconteça ANTES do salto.

---

## ITEM 3 — Core0 preso no loop de poll 0xb000d4a8

### Evidência (disassembly real do APPS.bin, ARM)
```
0xb000d494: bl   0xb000c720        ; r0 = descriptor
0xb000d498: ldr  r3, [r0, #0xc8]   ; lê campo de status
0xb000d49c: bic  r2, r3, #0x3fc
0xb000d4a0: bic  r2, r2, #3        ; r2 = status mascarado
0xb000d4a8: ldr  r3, [r4]          ; \ incrementa contador
0xb000d4ac: add  r3, r3, #1        ;  |
0xb000d4b0: str  r3, [r4]          ; /
0xb000d4b4: tst  r2, #1            ; testa bit 0 de r2
0xb000d4b8: lsr  r2, r2, #1
0xb000d4bc: beq  0xb000d4a8        ; loop enquanto bit==0
```
pc oscila só entre d4a8/b0/b8 do ciclo 20 ao fim. É popcount/poll: espera algum bit setado
em `[descriptor+0xc8]`. Como r2 vem de um campo que a base não atualiza, nunca sai.

### Causa raiz [infer]
`0xb000c720` retorna um descriptor cujo campo +0xc8 é um registro de status
(provavelmente device/IPC ready flag) que só é setado por um evento que ainda não ocorre —
possivelmente dependente do Core1 (item 1) ou de uma resposta de syscall/IPC.

### O que investigar
1. Descobrir o que `0xb000c720` retorna (thread desc? device handle?) e a origem de +0xc8.
2. Ver se +0xc8 é MMIO (então precisa de um device model setando o bit) ou RAM (então
   precisa de outra thread/IPC escrevendo).

### Esqueleto — sonda no descriptor + possível unblock
```cpp
// Hook de leitura em [descriptor+0xc8] para descobrir o endereço absoluto e a origem:
uc_hook hr;
uc_hook_add(core0_.uc, &hr, UC_HOOK_MEM_READ, (void*)probe_status_read,
            &core0_, /*begin*/0, /*end*/(uint64_t)-1);
// probe_status_read: se address == (r0_capturado + 0xc8), logar o valor e QUEM deveria
//    escrever. Se for MMIO, registrar um device stub que seta o bit após o evento certo.

// NÃO fazer: forçar r2=1 cegamente (mascara o bug, gera "false progress" 5a).
// O bit precisa vir do produtor real (Core1 REX ou resposta de IPC).
```
IMPORTANTE: resistir à tentação de patchar o bit. Se o item 1 (Core1) for resolvido, este
loop pode destravar sozinho — teste a hipótese antes de hackear.

---

## ITEM 4 — Loader/dispatch de applet BREW (.mod / AEEMod_Load)

### Evidência
- `grep AEEMod_Load|AEEShell|applet|\.mod` em zeebo_lle_main.cpp = **VAZIO**. Não existe
  loader de applet no orquestrador.
- ROADMAP Fase 7: `zbtest.mod` (SDK oficial) foi executado e mapeado até `AEEMod_Load`,
  mas isso foi no `zeebo_lle_mod_probe` (sonda isolada), NÃO no boot integrado.
- O "vetor BREW/AEECShell 0x10c874f4" do commit recente é a AEECShell subindo; um jogo
  Zeebo é um `.mod` que a AEECShell carrega via `ISHELL_CreateInstance` → `AEEMod_Load`.

### O que falta
Um caminho para: (a) a AEECShell pedir um applet por ClassID, (b) o loader resolver o
`.mod` do jogo (do sistema de arquivos EFS ou de um path injetado), (c) chamar o entry do
módulo (`AEEMod_Load` → `AEEClsCreateInstance`), (d) rodar o applet event loop
(`EVT_APP_START`, `EVT_KEY`, etc).

### Esqueleto — módulo de loader BREW (novo arquivo, ex. zeebo_brew_loader.h)
```cpp
// Estrutura mínima do dispatch BREW. Endereços a CONFIRMAR por RE do APPS.bin.
struct BrewLoader {
    uc_engine* uc;
    u32 aeemod_load_va = 0;   // [infer] resolver via RE: símbolo AEEMod_Load
    u32 ishell_create_va = 0; // ISHELL_CreateInstance

    // Interceptar a AEECShell quando ela chamar CreateInstance com o ClassID do jogo.
    // Hook de CODE no entry de ISHELL_CreateInstance:
    static void on_create_instance(uc_engine* uc, uint64_t addr, uint32_t sz, void* u) {
        u32 clsid; uc_reg_read(uc, UC_ARM_REG_R1, &clsid); // [infer] ABI: r1=ClassID
        printf("[BREW] ISHELL_CreateInstance(clsid=0x%08x)\n", clsid);
        // Se clsid == ClassID do .mod alvo, garantir que o módulo está carregado na EFS
        // ou injetar a imagem do .mod e redirecionar AEEMod_Load pra ela.
    }

    // Carregar um .mod do host para a memória guest (formato: ELF ARM ou MOD BREW):
    bool inject_mod(const std::string& host_path, u32 load_va) {
        std::vector<u8> mod = read_file(host_path);
        // Validar header (.mod BREW = ELF ARM com entry AEEMod_Load exportado)
        uc_mem_write(uc, load_va, mod.data(), mod.size());
        printf("[BREW] .mod injetado em 0x%08x (%zu bytes)\n", load_va, mod.size());
        return true;
    }
};
```
Investigação necessária ANTES de codar: extrair de APPS.bin os VAs de `AEEMod_Load`,
`ISHELL_CreateInstance`, `AEEClsCreateInstance` (símbolos ou por assinatura). O
`zeebo_lle_mod_probe` já mapeou o "ponto de despacho para AEEMod_Load" (ROADMAP 7) —
reaproveitar esse endereço.

### Dependência
Isto SÓ importa depois que Core0 rodar user-space de verdade (itens 1-3). Antes disso a
AEECShell não executa. Mas o loader pode ser desenvolvido/testado em paralelo com o
`zeebo_lle_mod_probe` isolado.

---

## ITEM 5 — Frame loop integrado (GPU real → display)

### Evidência
- `run_interleaved` já tem um caminho de display (linha ~452): quando `gpu_->is_fb_dirty()`,
  ele chama `sink_->update_frame(fb_buffer)`. MAS o conteúdo é um **test pattern gerado no
  loop** (`col = ((x>>3)&0x1F)<<11 | ...`), não a saída do rasterizer.
- O GPU real (`IGpuRasterizer`/`IglHook`, Fase 9) está integrado ao build e verificado por
  pixel ISOLADAMENTE (igl_smoke, gpu_display_integration), mas **não conectado ao
  Unicorn/guest** — ROADMAP 9 marca isso `[ ]` explicitamente ("Ligar GuestMachine ao
  Unicorn... só quando MAP_CONTROL destravar execução real de guest").

### O que falta
1. Ligar a vtable IGL/IEGL do guest (quando um applet chama gl*) ao `IglHook` → rasterizer
   → `fb_buffer`. Hoje nada submete GL porque nenhum applet roda.
2. Trocar o test pattern pelo framebuffer real do rasterizer.
3. Cadência de frame: hoje o present é acoplado a `is_fb_dirty` no loop de scheduler. Para
   jogo, precisa de um ritmo estável (~30fps) alinhado ao MDDI.

### Esqueleto — substituir o test pattern pela saída real
```cpp
// No run_interleaved, trocar o bloco de test pattern por:
if (gpu_ && gpu_->is_fb_dirty()) {
    gpu_->clear_fb_dirty();
    const u16* real_fb = gpu_->framebuffer(); // RGB565 640x480 do IGpuRasterizer
    if (real_fb) {
        sink_->update_frame(real_fb);         // sem test pattern
        printf("[Display] frame real (Adreno draws=%u)\n", gpu_->draws());
    }
}

// E ligar a vtable IGL do guest ao hook (uma vez, no setup), guardado atrás de um
// gate "guest_running" pra não disparar durante o boot:
if (guest_reached_userspace_) {
    igl_hook_.bind_to_guest_vtable(core0_.uc, igl_vtable_va_); // [infer] va da vtable IGL
}
```

### Dependência
Totalmente a jusante de 1-4. Sem applet rodando, não há chamada GL. É o ÚLTIMO item.
Mas a conexão test-pattern→real_fb pode ser feita já (barata) para ficar pronta.

---

## Resumo executivo p/ o agente da base

| # | Item | Bloqueio | Custo | Faça |
|---|------|----------|-------|------|
| 1 | Core1 sem REX | entry 0x00a00000 cru, sem preâmbulo reset | médio | achar reset vector real + setar CPSR/SP/vetores + slide-detector |
| 2 | MAP_CONTROL | mapeia páginas vazias (loader não populou) | médio | validar conteúdo da página no map_one; garantir cópia da task antes do salto |
| 3 | Core0 loop d4a8 | espera bit [desc+0xc8] nunca setado | baixo* | sondar origem do bit; NÃO forçar; testar se item 1 destrava |
| 4 | Loader BREW | inexistente no orquestrador | alto | módulo BrewLoader; reusar VA de AEEMod_Load do mod_probe |
| 5 | Frame loop | GPU real não ligado ao guest; test pattern | baixo | trocar test pattern por gpu_->framebuffer(); bind vtable IGL gated |

(*) item 3 pode ser "grátis" se item 1 resolver a origem do bit.

Ordem recomendada: 1 → 2 → 3 (juntos destravam o boot user-space) → 4 → 5.
Os itens 1, 2, 3 são o CORAÇÃO — depois deles, um jogo tem chance real de carregar.
Regra de ouro (Rafael): validar por BYTES/branches reais, nunca por insn-count; um pc que
sobe +4 constante ou um shim retorno-sucesso é derail, não progresso.

Do lado QDSP5 (meu, isolado): pronto e aguardando. No instante em que o Core0 rodar
user-space e o Core1 escalonar o REX, o primeiro RPC de áudio (AUDMGR 0x30000013 ou
ADSPRTOSATOM 0x3000000a) é capturado pelo hook Q0.1 — que fecha o bit de conclusão, os
args do proc 9 e o disc-map de uma vez.
