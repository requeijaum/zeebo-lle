# GPU_TODO.md — Estratégia gráfica do zeebo-lle (Adreno 130 / BREW OpenGL ES 1.1)

Status: PLANO. Escrito 2026-09-07. Ancorado no corpus local (fontes citadas por caminho).

> **Atualização 2026-09-12**: o freeze de desenvolvimento em `tools/cpp/gpu/` foi
> levantado por decisão do Rafael em 2026-09-11 (idem `tools/cpp/qdsp5/`). O plano
> segue válido como estratégia; trabalho ativo na pasta está autorizado, com os gates
> descritos aqui (verificar por bytes/efeito, nunca por "retornou sucesso").
Regra de ouro herdada: verificar execução por bytes/efeito, nunca por "retornou sucesso"
(ver notes/FINDINGS.md sessão 5a — o Adreno atual é falso: conta packets, não desenha).

═══════════════════════════════════════════════════════════════════════════════
0. TL;DR — a decisão que estressa tudo
═══════════════════════════════════════════════════════════════════════════════
O Zeebo expõe 3D ao jogo via BREW **IGL/IEGL** = OpenGL ES 1.1 Common Profile
(fixed-function, SEM shaders). A GPU física é o **Adreno 130 (Yamato/Z430)**, um
renderizador **tile-based/binning** com ring buffer PM4 sobre AXI.

Existem DOIS pontos de interceptação possíveis no LLE. A escolha certa NÃO é a
óbvia:

  (A) LLE-fundo — decodificar o ring buffer PM4 do Adreno em 0xA0000000 → pixel.
  (B) IGL-boundary — interceptar a vtable IGL/IEGL DENTRO do firmware e reemitir
      para OpenGL do host. (HLE cirúrgico embutido num núcleo LLE.)

>>> RECOMENDAÇÃO FORTE: fazer (B) primeiro como "GPU HLE hook" e deixar (A) como
    pesquisa de fidelidade de longo prazo. Justificativa dura (corpus):
    - docs/hardware-map.md §GPU: "256 KB SRAM too small → screen rendered in
      subsections; 8 bins @ VGA ... Packets may contain indirect references
      (reusable command lists)". Implicação citada textual: "HLE-via-GLES
      sidesteps all of this; LLE would need the binning + ring-buffer model."
      => (A) exige reimplementar binning + PM4 + indirect buffers ANTES de ver
      1 pixel. Meses de trabalho sem tela.
    - sources/zeebulator/core/brew/gl_hle.h:14: o `.mod` do jogo **linka
      estaticamente** o wrapper EGL_1x.c/GLES_1x.c da Qualcomm, que despacha
      TODA chamada gl*/egl* por **gpIGL/gpIEGL** — ponteiros globais colocados
      uma vez no startup. => Há uma vtable única, estável, bem-definida onde
      interceptar. É a MESMA fronteira que Zeebulator/Zeemu usam, mas aqui o
      guest ARM REAL a chama (não um reimplemento HLE do applet).
    - Isso não viola o espírito LLE do projeto: o firmware, o L4e, o AEEShell e
      a lógica do jogo continuam rodando como ARM real no Unicorn. Só o driver
      GL do Adreno (que traduziria para PM4) é curto-circuitado para host GL —
      exatamente o que Citra/Dolphin fazem no modo "HW renderer".

═══════════════════════════════════════════════════════════════════════════════
1. Como emuladores de console procedem com gráficos (as 3 camadas)
═══════════════════════════════════════════════════════════════════════════════
Interceptação, do mais baixo ao mais alto:

  1. LLE de GPU (comando→pixel). Ler ring buffer, decodificar cada packet
     (estado/vértices/textura/draw) e rasterizar por SW ou traduzir p/ GPU host.
     Ex.: PCSX2(GS), Dolphin(Flipper/TEV), RPCS3(RSX→Vulkan). Fidelidade máxima,
     custo máximo. => é o que o Adreno DEVERIA fazer p/ ser real (caminho A).

  2. HLE de GPU / tradução de API. Interceptar a API gráfica (GL ES) e reemitir
     p/ OpenGL/Vulkan host. Ex.: Zeebulator gl_hle.cpp, Zeemu QXGL/BrewEGL.
     => caminho B. Guest roda a lógica; host roda o GL.

  3. Framebuffer-only. Só apresentar o buffer final. É onde o MDDI do LLE está
     hoje (zeebo_mddi_display.cpp + zeebo_fb_sink.cpp). Basta p/ 2D por-CPU,
     insuficiente p/ 3D.

REFERÊNCIA-CHAVE = **3DS/Citra**: GPU PICA200 (GL-ES-2-like, pipeline
semi-fixo) ≈ Adreno 130 (GL ES 1.1). Citra oferece DOIS renderers atrás de uma
`RasterizerInterface`: software (LLE fiel) + hardware OpenGL/Vulkan (traduz
registradores PICA→GL host). **Esse "duas implementações atrás de uma interface"
é o padrão a copiar.** OpenGL ES importa porque (i) é a API que o próprio Zeebo
expõe (IGL) e (ii) GL ES 1.1 é ~subconjunto de GL 1.x/2.x desktop → tradução
barata.

═══════════════════════════════════════════════════════════════════════════════
2. Estado atual do zeebo-lle (verificado no código) — o que existe e o que mente
═══════════════════════════════════════════════════════════════════════════════
- tools/cpp/zeebo_adreno130_gpu.cpp — STUB. read() devolve CHIP_ID Yamato
  (0x01030000)/STATUS/ponteiros RB. process_ring_buffer() (linha 107) é FALSO:
  faz `draw_calls += (wptr-rptr)` e `rptr=wptr`. NÃO lê rb_base_, NÃO decodifica
  packet, NÃO desenha. É um contador. [DÍVIDA — marcar como não-funcional]
- tools/cpp/zeebo_mddi_display.cpp — transporta framebuffer RGB565 640x480 (core
  ver 0x00000102) via descritor DMA. Mostra VRAM pronta; não gera imagem.
- tools/cpp/zeebo_fb_sink.cpp — SDL2 + PPM/BMP export de RGB565 640x480. OK.
- BLOQUEIO A MONTANTE: notes/FINDINGS.md 5a — o LLE trava no MAP_CONTROL (shim
  retorno-sucesso); nenhum código de jogo executa ainda. Logo NÃO há guest
  submetendo GL/PM4 p/ testar gráfico. Ver §7 (ordem de trabalho).

═══════════════════════════════════════════════════════════════════════════════
3. Fatos de hardware do corpus (usar como spec, todos [CONF])
═══════════════════════════════════════════════════════════════════════════════
(docs/hardware-map.md salvo localmente)
- GPU: Adreno 130 integrado, classe GL ES 1.0/1.1, Q3Dimension/Imageon.
- Perf console-específica (DevGuide, supersede wiki): 1.6 M tri/s; fill ~63 M
  texels/s; VGA 640x480 4:3 ONLY.
- Arquitetura: **tile-based binning**, SRAM 256 KB → 8 bins @ VGA (2 @ QVGA).
- Ring buffer: CP_WPTR sobre AHB lento; GPU lê Mem_WPTR via AXI, consome stream,
  atualiza Mem_RPTR; packets podem ter indirect refs (command lists reusáveis).
- MMIO: 0xA0000000 HW3D graphics; 0xAA200000 MDP (display) [HLE: skip];
  0xAA500000–0xAA700000 MDDI client/host.
- Extensões GL ES 1.1 HW-aceleradas (DevGuide §7.2) — o ESCOPO EXATO do IGL:
    SIM: ARB_vertex_buffer_object (VBO), OES_draw_texture, OES_matrix_palette,
         multitexture, OES_point_size_array, OES_point_sprite,
         ARB_texture_env_combine, ARB_texture_env_dot3.
    NÃO: SGIS_generate_mipmap, OES_matrix_get, ARB_point_parameters,
         render-to-texture, user clip planes.
  => Implementar SÓ o subconjunto "SIM". Se um jogo usar algo "NÃO", é bug dele
     ou não usa. Não gaste tempo no que o HW não tinha.

═══════════════════════════════════════════════════════════════════════════════
4. A fronteira IGL/IEGL (caminho B) — mapa de entrypoints do corpus
═══════════════════════════════════════════════════════════════════════════════
Fonte primária local: sources/zeebulator/core/brew/gl_hle.{h,cpp} (GPLv3) e
sources/zeemu/brew/BrewQXGL*.{h,cpp} + BrewEGL.cpp + graphics/RenderBackend.h.
USO: como LISTA DE CONTRATO (quais funções existem, que args, que semântica).
NÃO copiar código — reimplementar; ambos são GPL, o LLE é nosso. Clean vs a1Sim:
esta ABI é PÚBLICA (AEEGL.h/AEEIDisplay.h), então zero risco de contaminação.

EGL (handles = inteiros sentinela; 1 contexto host real por baixo):
  eglGetError, eglGetDisplay, eglInitialize, eglQueryString(EGL_EXTENSIONS →
  alimenta decisões do jogo, ver PHASE8_LOG do Double Dragon), eglChooseConfig,
  eglCreateWindowSurface, eglDestroySurface, eglCreateContext, eglDestroyContext,
  eglMakeCurrent, eglSwapBuffers (=fim de frame → present).

GL ES 1.1 fixed-function, agrupado por trabalho:
  - Frame/estado: glClear, glClearColorx, glViewport, glEnable/glDisable,
    glDepthFunc/glDepthMask, glBlendFunc, glAlphaFunc, glCullFace, glShadeModel,
    glColor4x.
  - Matriz (pilha fixed-function, SEM shader): glMatrixMode, glLoadIdentity,
    glLoadMatrixx, glPushMatrix/glPopMatrix, glTranslatex/glRotatex/glScalex,
    glFrustumx/glOrthox.
  - Vertex arrays (o coração do draw): glVertexPointer, glColorPointer,
    glTexCoordPointer, glNormalPointer, glEnableClientState/glDisableClientState,
    glDrawArrays, glDrawElements.
    >>> PONTEIROS SÃO ENDEREÇOS ARM EMULADOS, não host. No draw, LER a memória
        guest, converter cada componente p/ float host pela semântica GL ES 1.x
        por-tipo (ver gl_types.h ReadGlComponent do zeebulator) → montar
        GlVertexArrays host-nativo → DrawArrays host. glDrawElements = expandir
        índices p/ arrays equivalentes (zeebulator faz assim; não-indexado no
        host — trade-off conhecido e aceitável no bring-up).
  - Textura: glGenTextures/glDeleteTextures/glBindTexture, glTexParameterx,
    glTexImage2D (copiar pixels da mem guest p/ buffer host pelo (format,type,
    w,h) real), glCompressedTexImage2D (ATITC — ver §5), glActiveTexture +
    glClientActiveTexture (multitexture), glTexEnvx (combine/dot3).
  - I3D fixed-function (luz/material): glLightfv, glMaterialfv, glShadeModel —
    shapes em RenderBackend.h guest_gl_light/guest_gl_material do Zeemu.

Convenções de vtable (brew-abi.md, confirmado vs AEEGL.h/AEEIDisplay.h):
  - O wrapper GLES_1x.c chama via macro IGL_glX(p,a) = AEEGETPVTBL(p,IGL)->glX(a)
    — passa só os args, não `p`. => No hook, o `p`(IGL*) é implícito por thread;
    recuperar contexto por gpIGL.
  - IDisplay: 26 slots reais; SetClipRect=slot18 (off 0x48). 2D BREW (DrawRect/
    DrawText/Update) é caminho SEPARADO do IGL — cobre menus/HUD. Loop por-frame
    real (DD): GETUPTIMEMS→4×MEMSET→SetClipRect(NULL)→DrawRect→SetColor→6×[STRLEN
    →strcpy→DrawText]→SetTimer re-arm. Timers ONE-SHOT (jogo se re-arma).

═══════════════════════════════════════════════════════════════════════════════
5. Texturas & formatos (corpus docs/file-formats.md) — pipeline de upload
═══════════════════════════════════════════════════════════════════════════════
- **ATITC** (ATI Texture Compression, era Adreno 130): header lido pelo PRÓPRIO
  jogo (u32 sig LE, width, height, flags, dataOffset), payload passado a
  glCompressedTexImage2D. O host desktop NÃO tem ATITC → decodificar host-side
  p/ RGBA8 e forwardar como upload normal (zeebulator core/loader/atitc.cpp;
  ref SDK simple_atitc.c). [CONF]
- **.obm1** sprite/textura paletizada; **.bar** archive de recursos (PopCap-
  style; RIFF/ID3/PNG caem nos offsets); **.ggz/.pakz/.pkg** archives de asset;
  **PAK** = 2 formatos distintos (Quake clássico vs Onan). Texturas chegam ao
  IGL já descomprimidas pelo jogo OU via glCompressedTexImage2D(ATITC).
- Framebuffer final: RGB565 640x480 → MDDI → fb_sink. O present do host GL
  (caminho B) substitui/coexiste: renderizar em FBO 640x480, ler RGB565, entregar
  ao MESMO fb_sink (não duplicar a janela).

═══════════════════════════════════════════════════════════════════════════════
6. Arquitetura-alvo (padrão Citra: interface + 2 backends)
═══════════════════════════════════════════════════════════════════════════════
Criar tools/cpp/gpu/ com:

  IGpuRasterizer (interface abstrata — o "RasterizerInterface" do Citra):
    setViewport, clear(color/depth), setState(blend/depth/alpha/cull/shade),
    setMatrix(mode,4x4), bindTexture, texImage2D(fmt,w,h,data),
    drawArrays(prim, GlVertexArrays), present(rgb565 out).

  Backend 1 — GlHostRasterizer (caminho B, PRIMEIRO): traduz p/ OpenGL desktop
    (ou GL ES no host via ANGLE). Fixed-function GL ES 1.1 → ou GL 1.x fixed
    pipeline (mais direto) ou um shader ubershader que emula combine/dot3/luz.
    Presa em FBO 640x480 → glReadPixels RGB565 → fb_sink existente.

  Backend 2 — SoftRasterizer (caminho A / fidelidade, DEPOIS): rasterizador SW
    triângulo-a-triângulo, sem depender de GPU host (bom p/ headless/CI e p/
    validar contra o HW). Espelha o software renderer do Citra.

  Fonte dos comandos — DOIS alimentadores para a MESMA interface:
    (B) IglHook: intercepta a vtable IGL/IEGL no firmware (via gpIGL) e chama
        IGpuRasterizer direto. É o que roda primeiro (barato, testável).
    (A) Pm4Decoder: quando/se quisermos fidelidade total, decodificar o ring
        buffer do Adreno (0xA0000000, rb_base_) — packets tipo-0 (write reg) e
        tipo-3 (DRAW_INDX, load state, textura) — e emitir na MESMA interface.
        Rosetta ABERTA p/ o formato: freedreno/mesa a2xx (registradores Yamato/
        A2xx = mesma família). Substitui o process_ring_buffer() falso atual.

  => Uma interface, dois produtores (IGL hook / PM4), dois consumidores (GL host /
     SW). Qualquer combinação testável isoladamente.

═══════════════════════════════════════════════════════════════════════════════
7. Ordem de trabalho (dependências reais — NÃO construir telhado sem parede)
═══════════════════════════════════════════════════════════════════════════════
FASE 0 (PRÉ-REQUISITO, fora deste doc mas bloqueia tudo):
  - Resolver MAP_CONTROL real (FINDINGS 5a): sem execução de guest, nenhum
    caminho gráfico tem o que consumir. NADA de gráfico é testável antes disso.
    >>> Não decodifique PM4 nem escreva IglHook contra hardware antes de haver 1
        applet executando de verdade. Seria repetir o erro do Adreno-contador.

FASE 1 (esqueleto, pode começar já — sem guest, com testes sintéticos):
  - [ ] Criar IGpuRasterizer + GlHostRasterizer + SoftRasterizer (stubs reais).
  - [ ] Ligar present → fb_sink (RGB565 640x480) já existente. Teste: clear azul
        → PPM azul. (efeito real, não contador.)
  - [ ] Marcar zeebo_adreno130_gpu.cpp process_ring_buffer() como DEPRECATED/
        FALSO no header, apontando p/ este doc.

FASE 2 (IglHook — o primeiro pixel de jogo real):
  - [ ] Localizar gpIGL/gpIEGL no firmware (símbolo/relocação; o wrapper
        GLES_1x.c os seta no startup). Confirmar por disassembly ARM.
  - [ ] Hookar a vtable IGL: cada slot → método IGpuRasterizer. Começar por
        eglMakeCurrent/glViewport/glClear/glClearColorx/eglSwapBuffers (basta p/
        ver a tela limpar e trocar frame).
  - [ ] Vertex arrays + glDrawArrays/Elements: ler mem guest, converter por-tipo,
        montar arrays host. Primeiro triângulo de jogo.
  - [ ] Texturas: glTexImage2D → upload; glCompressedTexImage2D → decode ATITC.
  - [ ] Multitexture + combine/dot3 (extensões "SIM" do §3).
  VERIFICAÇÃO (regra de ouro): validar por FRAMEBUFFER (pixels certos no PPM),
  nunca por "chamada retornou". Comparar frame contra Infuse/Zeebulator no MESMO
  jogo (oráculo caixa-preta).

FASE 3 (2D BREW paralelo): IDisplay DrawRect/DrawText/Update/SetClipRect →
  compõe menus/HUD sobre (ou sob) o frame GL. Loop por-frame do §4.

FASE 4 (fidelidade LLE, opcional/longo prazo): Pm4Decoder do ring buffer Adreno
  + binning tile-based → alimenta a MESMA IGpuRasterizer. Só se houver razão
  (jogo que fale direto com o HW3D sem passar pelo IGL, ou meta de precisão).
  Custo alto; ganho só de fidelidade. Ref: freedreno a2xx.

═══════════════════════════════════════════════════════════════════════════════
8. Riscos, pitfalls e decisões a estressar
═══════════════════════════════════════════════════════════════════════════════
- ARMADILHA DO CONTADOR (já cometida no Adreno): "processou N packets/draws" NÃO
  é progresso. Só framebuffer com pixels corretos conta. Todo TODO acima exige
  efeito visual verificável.
- Ponteiros guest vs host: TODO ponteiro de array/textura vindo do IGL é VA ARM.
  Ler via Unicorn no momento do draw. Nunca deref direto.
- Fixed-function no host moderno: GL desktop core removeu fixed pipeline. Opções:
  (i) contexto GL compat 1.x (simples, deprecated), (ii) ANGLE/GL ES no host,
  (iii) ubershader que emula matriz+combine+dot3+luz. Decidir na FASE 1.
  Recomendo (iii) p/ portabilidade (mesmo destino do Citra HW renderer).
- glDrawElements→expansão de índices: perde reuso de vértice; ok no bring-up,
  otimizar depois (VBO real). [CONF trade-off zeebulator]
- ATITC ausente no host: SEMPRE decodificar host-side p/ RGBA8. Não existe
  extensão desktop equivalente confiável.
- VGA fixo 640x480 4:3: não implementar resize/aspect além disso; HW não tinha.
- Binning (caminho A): 8 bins @ VGA com SRAM 256KB — se algum dia decodificar
  PM4, o binning é OBRIGATÓRIO p/ fidelidade de ordem/scissor; caminho B ignora
  (o host tem framebuffer inteiro).
- Oráculo de validação: Zeebulator (GPLv3, ARM interp, ~45 testes, gl_hle +
  gl_lifecycle_test) é o melhor p/ cross-check de comportamento GL SEM risco
  clean-room. Infuse = oráculo caixa-preta de imagem. a1Sim = INÚTIL p/ gráfico
  (não tem ARM nem GL host; ver FINDINGS 5c).

═══════════════════════════════════════════════════════════════════════════════
9. Fontes locais (todas neste corpus)
═══════════════════════════════════════════════════════════════════════════════
- ~/projects/zeebo-emulator/docs/hardware-map.md  (Adreno arch, GL ES §7.2, MMIO)
- ~/projects/zeebo-emulator/docs/brew-abi.md       (IGL/IEGL vtable, IDisplay 2D)
- ~/projects/zeebo-emulator/docs/file-formats.md   (ATITC, .bar/.obm1/.ggz/.pakz)
- ~/projects/zeebo-emulator/research/sources/zeebulator/core/brew/gl_hle.{h,cpp}
- ~/projects/zeebo-emulator/research/sources/zeebulator/core/loader/atitc.{h,cpp}
- ~/projects/zeebo-emulator/research/sources/zeemu/brew/BrewQXGL*.{h,cpp}, BrewEGL.cpp
- ~/projects/zeebo-emulator/research/sources/zeemu/graphics/RenderBackend.{h,cpp}
- ~/projects/zeebo-lle/tools/cpp/zeebo_adreno130_gpu.cpp  (stub a substituir)
- ~/projects/zeebo-lle/tools/cpp/zeebo_mddi_display.cpp, zeebo_fb_sink.cpp
- ~/projects/zeebo-lle/notes/FINDINGS.md  (5a MAP_CONTROL bloqueio; 5c a1Sim)
- Externo p/ caminho A: freedreno/mesa a2xx (registradores Yamato/A2xx, PM4).

═══════════════════════════════════════════════════════════════════════════════
10. AUDITORIA DO ESQUELETO (Fase 1) + ShadPS4/PM4 — 2026-09-07
═══════════════════════════════════════════════════════════════════════════════
Esqueleto criado e VERIFICADO POR EXECUÇÃO (não por "compilou"):
  tools/cpp/gpu/{igpu_rasterizer.h, soft_rasterizer.cpp, gl_host_rasterizer.cpp,
                 rasterizer_factory.cpp, pm4_adreno.h, gpu_smoke.cpp, Makefile.gpu}
  - Build ISOLADO (Makefile.gpu próprio) — NÃO toca Makefile/código do outro agente.
    git status = apenas "?? tools/cpp/gpu/" (zero modificação em arquivos rastreados).
  - Dois modos: `make -f Makefile.gpu` (software headless, sem dep GL) e
    `gpu_smoke_gl` (-DZEEBO_GL_HOST, stub GL compila; sem -lGL até corpos Fase 2).
  - gpu_smoke prova por FRAMEBUFFER: T1 clear vermelho center=0xF800 PASS;
    T2 triângulo verde center=0x07E0 PASS; T3 pm4-walk packets/regs/draws PASS.

ShadPS4 (fonte primária consultada: src/video_core/amdgpu/{pm4_opcodes.h,
liverpool.cpp,pm4_cmds.h}):
  - Modelo de walk reaproveitado: laço `while(i<n)` lê header dword, type=bits[31:30];
    type0 escreve count+1 regs sequenciais; type3 = {opcode bits[15:8], count bits
    [29:16]}, avança count+1; type2 = NOP 1-dword. É o MESMO PM4 "igual ao radeon".
  - NÃO reaproveitável: a tabela IT_* do ShadPS4 é GCN (Liverpool), opcodes
    diferentes (DrawIndex2=0x27 GCN vs DrawIndx=0x22 Adreno). Colar seria erro.

CORREÇÕES aplicadas após checagem contra FONTE PRIMÁRIA
(drivers/gpu/msm/adreno_pm4types.h do kernel MSM + freedreno wiki Command-Stream):
  [ERRO] Load_Reg_Group=0x33          -> REMOVIDO (0x33 não é opcode válido).
  [ERRO] Set_Shader_Consts=0x2E       -> nome errado; 0x2e = CP_LOAD_CONSTANT_CONTEXT.
  [ERRO] Set_Constant tratado como pares (offset,val) -> layout REAL: DATA_1 =
         0x00040000|(BASE-0x2000) seguido de N-1 valores sequenciais. Corrigido.
  [OMISSÃO GRAVE] CP_INDIRECT_BUFFER (0x3F) não recursava -> em GPU tile-based os
         draws reais vivem em IBs por bin; sem recursão TODOS os draws se perdiam.
         Adicionado callback resolve_ib (VA->host) + recursão pm4_walk. resolve_ib
         devolve {nullptr,0} até MAP_CONTROL fornecer a tradução (honesto).
  [ADICIONADO] CP_SET_BIN_MASK/SELECT (0x50/0x51) — binning/tile, essencial p/
         fidelidade do Adreno (hardware-map.md: 8 bins @ VGA, SRAM 256KB).

RISCO DE PROVENIÊNCIA (o mais importante desta auditoria)
  - O Adreno 130 é ATI **Imageon (linhagem Z430/Z180), ANTERIOR ao A2xx (=Adreno
    200)**. Confirmado no corpus (tripleoxygen wiki, KNOWLEDGE.md) e Wikipedia/
    HandWiki (Qualcomm licenciou Imageon da AMD; comprou a divisão em jan/2009).
  - freedreno documenta A2xx+; o Imageon 130 pode ter tabela IT_OPCODE divergente.
    => Os opcodes A2xx acima são HIPÓTESE de trabalho, marcados como tal no header.
    Validação real só contra um ring buffer REAL do Zeebo — que depende do
    MAP_CONTROL (FINDINGS 5a). Não declarar a tabela correta antes disso.

DECISÃO GL host (registrada no gl_host_rasterizer.cpp)
  - Esboço assume ubershader emulando fixed-function GLES 1.1 (matriz+combine+dot3+
    luz), por portabilidade — mesmo destino do HW renderer do Citra. alpha_test
    (inexistente em GL core) -> discard no shader. draw_indexed usa glDrawElements
    real (vantagem sobre a expansão de índices do caminho HLE do Zeebulator).

BLOQUEIO DE MONTANTE INTACTO (regra de ouro)
  - Nada acima roda contra hardware: sem MAP_CONTROL não há guest submetendo ring
    buffer. gpu_smoke usa stream SINTÉTICO de propósito. Fase 0 (MAP_CONTROL)
    continua sendo o pré-requisito — não construir o telhado antes da parede.

═══════════════════════════════════════════════════════════════════════════════
12. ATUALIZAÇÃO AUDITADA (2026-09-11) — Integração IGL Hook e Rasterizer
═══════════════════════════════════════════════════════════════════════════════
- Desenvolvimento em GPU liberado formalmente por Rafael (2026-09-11).
- O IGL hook (`gpu/igl_hook.cpp`) está totalmente integrado no executável principal
  e no runner do Zeetris (`zeebo_zeetris_runner.h`). A interceptação captura 80 slots
  de OpenGL ES 1.1 da vtable do guest.
- Comportamento de binding verificado: no Zeebo, `glVertexPointer` e correlatos
  realizam o BIND do array no contexto do driver.
- SoftRasterizer emite geometria real para o framebuffer (660 chamadas IGL por loop de
  teste; geometria medida de 77.120 px / ~25.1% de cobertura de tela em `test-zeetris-runner`).
- PENDÊNCIA IMEDIATA DA GPU: suporte à carga e decodificação de texturas (rotina
  `0x120056fc` do Zeetris), atualmente não alcançada pela máquina de estados do jogo.


═══════════════════════════════════════════════════════════════════════════════
11. EMULADORES NINTENDO vs Adreno 130 (quais servem) — 2026-09-07
═══════════════════════════════════════════════════════════════════════════════
Pergunta: Dolphin/Cemu/Ryujinx têm algo útil p/ a GPU quase-AMD do Zeebo?
Resposta curta: CEMU é ouro (mesma família PM4/AMD); DOLPHIN é ouro p/ o
fixed-function (GLES 1.1); Ryujinx/Suyu/Eden NÃO servem (Nvidia Tegra, não-PM4).

── CEMU (Wii U / GPU "Latte" = AMD R700) — O MAIS RELEVANTE ────────────────────
Fonte primária lida: src/Cafe/HW/Latte/Core/{LattePM4.h, LatteCommandProcessor.cpp,
LatteRingBuffer.cpp, LatteSoftware.cpp} e ISA/LatteInstructions.h.
  1) MESMO formato de header type3 que o nosso decoder:
       pm4HeaderType3 = 0xC0000000 | (itCode<<8) | ((nDWords-1)<<16)
     -> confirma NOSSA decodificação (op em bits[15:8], count=(N-1)+1). ✔
  2) MAS os VALORES de opcode do R700 diferem dos do A2xx E dos do GCN(ShadPS4):
       R700:  DRAW_INDEX_2=0x27  DRAW_INDEX_AUTO=0x2D  SET_CONTEXT_REG=0x69
              SET_RESOURCE=0x6D  SET_SAMPLER=0x6E  INDIRECT_BUFFER_PRIV=0x32
       A2xx:  DRAW_INDX=0x22     SET_CONSTANT=0x2D    (tabela própria)
       GCN :  DRAW_INDEX_2=0x27  SET_CONTEXT_REG=0x69 (parecido c/ R700, não igual)
     => CONFIRMA o alerta da seção 10: cada família AMD tem sua tabela IT_*.
        Nenhuma é copiável direto p/ o Imageon 130. Copiar seria bug.
  3) LIÇÃO DE ARQUITETURA (a mais valiosa): Cemu injeta opcodes HLE PRÓPRIOS no
     stream — IT_HLE_REQUEST_SWAP_BUFFERS(0xF0), IT_HLE_CLEAR_COLOR_DEPTH_STENCIL
     (0xF5), IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER(0xF3), etc. É EXATAMENTE o
     padrão "HLE-na-fronteira" do nosso GPU_TODO §0: interceptar operações de alto
     nível em vez de emular cada detalhe do CP. Um emulador em produção valida a
     nossa decisão central.
  4) LatteSoftware.cpp = rasterizador software de referência (espelha o nosso
     soft_rasterizer.cpp como caminho de correção/headless).
  Uso p/ o LLE: LattePM4.h/LatteCommandProcessor.cpp como MODELO do laço do CP e
  do padrão de IT_HLE_*; NÃO como tabela de opcodes (R700≠Imageon).

── DOLPHIN (GameCube/Wii — Flipper/Hollywood = ATI "ArtX") — OURO p/ FIXED-FUNC ─
  - Flipper NÃO tem shaders nem PM4: é pipeline FIXED-FUNCTION com TEV (Texture
    EnVironment) — combinadores de textura configuráveis. Isso é o ANÁLOGO EXATO
    do GLES 1.1 do Zeebo (também fixed-function: glTexEnv/combine/dot3/luz).
  - Dolphin resolveu "emular fixed-function em GL/Vulkan moderno" com UBERSHADERS
    (um shader gigante que interpreta o estado fixed-function em runtime). É
    precisamente a técnica que o nosso gl_host_rasterizer.cpp escolheu (seção 10).
    => Dolphin é a REFERÊNCIA de implementação do ubershader GLES1-em-GL-moderno.
  - NÃO tem parser PM4 (arquitetura de comando diferente: FIFO GX/CP do Flipper).
  Uso p/ o LLE: referência do ubershader/TEV->combine e do caminho de textura
  fixed-function; docs amnoid.de/gc/tev.html + blog Dolphin "Ubershaders".

── RYUJINX / SUYU / EDEN (Switch — Tegra X1 = Nvidia Maxwell) — NÃO SERVEM ──────
  - GPU Nvidia, NÃO-AMD. Command stream = GPFIFO + "methods"/macros (MME, um
    interpretador/JIT de macros), modelo totalmente diferente do PM4.
  - Nada de PM4, nada de fixed-function (Maxwell é unified shader moderno).
  - Único conceito tangencial: MME (macro JIT) ~ ideia de recompilar command
    macros — irrelevante p/ o Adreno 130. Descartar p/ este projeto.

VEREDITO
  Cemu  -> modelo do laço do Command Processor + padrão IT_HLE_* (valida §0).
  Dolphin -> modelo do ubershader p/ fixed-function GLES 1.1 (valida gl_host §10).
  Ryujinx/Suyu/Eden -> ignorar (arquitetura Nvidia não-PM4).
  Reforço: header PM4 idêntico entre A2xx/R700/GCN, mas TABELA de opcodes é por
  família — a do Imageon 130 continua a ser validada só contra ring buffer real
  (bloqueio MAP_CONTROL, FINDINGS 5a).

═══════════════════════════════════════════════════════════════════════════════
12. AUDITORIA DE PRESSUPOSTOS (o que ainda pode estar ERRADO) — 2026-09-07
═══════════════════════════════════════════════════════════════════════════════
Revisão crítica das seções 1-11 contra o corpus primário. Dois achados que
mudam o peso do plano:

[ERRO-1 / factual] Base de registradores trocada.
  - hardware-map.md (mapa físico ARM11): 0xA0000000 = "HW3D graphics".
  - 0xAA200000 (citado como pendência na seção 10) é o MDP = display/compositor,
    NÃO o núcleo 3D. O decoder do CP/3D deve olhar 0xA0000000; o MDP/MDDI é o
    caminho de SCANOUT (já modelado em zeebo_mddi_display.cpp). Não confundir.
  - Correção: base do 3D core = 0xA0000000 [CONF DevGuide, mapa físico]. A base
    exata dos regs CP_RB_* dentro desse bloco continua não fixada.

[ERRO-2 / estrutural, o mais importante] O modelo ring-buffer PM4 é CONJECTURA.
  - TODAS as linhas de hardware-map.md sobre "Ring buffer command stream",
    "CP_WPTR/Mem_RPTR", "tile-based/8 bins", "indirect references" estão marcadas
    [CONF] e [CONF analysis] — inferência do padrão MSM genérico, NÃO extraídas do
    dump do Zeebo nem confirmadas por RE.
  - Logo pm4_adreno.h (header type0/3, opcodes A2xx, indirect buffer, bin mask)
    está empilhado sobre DUAS hipóteses não verificadas:
      (a) que o Imageon 130 é dirigido por ring buffer PM4 (vs register-poke direto
          via MMIO, plausível numa peça Imageon Z1xx pré-CP-unificado);
      (b) que a tabela IT_OPCODE é A2xx (já rebaixada a hipótese na seção 10/11).
  - IMPACTO: se (a) for falsa, pm4_adreno.h é o arquivo errado — o produtor real
    seria um observador de escritas MMIO em 0xA0000000, não um walker de ring
    buffer. O restante do skeleton (IGpuRasterizer + soft/gl backends) permanece
    válido: a interface não depende de COMO os comandos chegam.
  - AÇÃO: manter pm4_adreno.h como HIPÓTESE explícita (renomear mentalmente para
    "produtor candidato A"); adicionar como TODO um "produtor candidato B" =
    MmioPokeObserver, decidido só quando houver execução real (MAP_CONTROL) p/
    observar se o guest escreve um ringbuffer ou faz register-poke.

[OMISSÃO-1] O caminho HLE-na-fronteira (§0, a decisão CENTRAL) NÃO depende de
  nenhuma das duas hipóteses acima. Hook em gpIGL/gpIEGL (Zeebulator gl_hle.h:14)
  intercepta ANTES do driver gerar qualquer comando de hardware. Ou seja: a Fase 1
  correta pode ser feita HOJE sem resolver PM4-vs-MMIO. pm4_adreno.h é FASE 4
  (LLE do CP) e só faz sentido depois — está adiantado no skeleton, o que é ok
  como esboço, mas NÃO deve virar prioridade antes do IglHook.
  => Reordenar: IglHook (produtor real da Fase 1) > IGpuRasterizer+backends (feito)
     >> pm4_adreno.h (Fase 4, hipótese, pós-MAP_CONTROL).

[OMISSÃO-2] Faltou verificar se o BREW GL do Zeebo é HW-acelerado ou software.
  DevGuide §7.2 lista extensões "Hardware-accelerated on Zeebo" [CONF DevGuide] —
  indica caminho HW real (logo há SIM um core 3D gerando comandos), o que sustenta
  a existência de um produtor de hardware a ser modelado na Fase 4. Não muda a
  ordem, mas confirma que a Fase 4 não é inútil.

RESUMO HONESTO DO ESTADO
  - Skeleton (interface + 2 backends + smoke): SÓLIDO, verificado por framebuffer.
  - pm4_adreno.h: ESBOÇO SOBRE CONJECTURA — útil como estudo, não como verdade.
    Corrigido header/opcodes vs fontes primárias (seções 10/11), mas o próprio
    pressuposto "existe ring buffer PM4" segue [CONF] até o MAP_CONTROL permitir
    observar o guest.
  - Prioridade real da Fase 1 = IglHook (HLE na fronteira), independente de PM4.

═══════════════════════════════════════════════════════════════════════════════
13. EVIDÊNCIA DO FIRMWARE REAL — resolve a conjectura da §12 — 2026-09-07
═══════════════════════════════════════════════════════════════════════════════
Fonte: strings do dump REAL nand/1.1.2_APPS.bin (não [CONF] — bytes do firmware).

FATOS (substituem hipóteses da §12):
  [F1] Linhagem confirmada ATI Imageon (NÃO A2xx genérico):
       extensões GL reais no firmware: GL_ATI_imageon_misc,
       GL_ATI_texture_compression_atitc, GL_ATI_extended_texture_coordinate_
       data_formats, GL_QUALCOMM_vertex_buffer_object. Lista de ext bate quase
       1:1 com DevGuide §7.2 (§hardware-map) — agora CONFIRMADA, não [CONF].
  [F2] Driver = "ATICORE", opera por BUFFER IDs, não ring buffer PM4 exposto:
       símbolos outputATIcoreBufferID, graphics_GetCurrentBufferID,
       IGRAPHICS_GetFillMode, "MP: error on creating IGraphics".
  [F3] O CAMINHO DE COMANDO REAL É VIA QDSP (DSP), não ARM11->ringbuffer 3D:
       QDSP_GRAPHICSTASK_MPUGRAPHICSCMDQUEUE_MAX_CMD_SIZE (uma CMD QUEUE p/ o DSP),
       "THE GRAPHICS DSP IMAGE IS READY AND RUNNING", "Graphics msg callback
       Message From DSP", Enabling/Disabling GRAPHICSTASK, "Ran Out of GRAPHICS
       Packets!". => O 3D é offload p/ uma imagem de firmware no QDSP5; a ARM11
       enfileira comandos MPU->DSP, não escreve registradores do CP diretamente.
  [F4] Composição/scanout = MDP overlay (ovimg), não framebuffer linear:
       OEM_graphics_update, mdp_update_ovimg_async, ovimgID/glSurf,
       overlay/underlay/window; EGL: eglCreateCompositeSurfaceQUALCOMM,
       eglGetColorBufferQUALCOMM. Confirma 0xAA200000=MDP como scanout (§12 F1).
  [F5] ATITC confirmado como formato de textura (valida GPU_TODO §5):
       GL_COMPRESSED_RGB(A)_ATI_TC citados como internalformat reais.

IMPACTO NO PLANO (decisivo):
  - A §12 perguntava "ring buffer PM4 vs register-poke MMIO". RESPOSTA REAL: é uma
    TERCEIRA opção — command queue MPU->QDSP5 (DSP faz o trabalho 3D). Logo
    pm4_adreno.h (walker de ring buffer A2xx) modela a camada ERRADA para LLE:
    não há PM4 A2xx observável do lado ARM11 — há mensagens p/ o DSP gráfico.
    => pm4_adreno.h REBAIXADO de "produtor candidato A" para "referência de
       estudo apenas". Um LLE real do 3D exigiria emular a imagem QDSP5 gráfica
       (enorme; provável beco, igual ao TBIN do DKWDRV).
  - REFORÇA definitivamente a decisão §0: HLE-na-fronteira-IGL é o único caminho
    são. O firmware fala OpenGL ES 1.1 padrão (gpIGL/gpIEGL) ACIMA do ATICORE/DSP;
    hookar nessa fronteira captura tudo e evita tanto o PM4 quanto o QDSP.
  - IglHook continua a Fase 1 real. pm4_adreno.h fica como esboço histórico.
  - Novo recurso p/ o backend GL host: SDK tem samples OpenGL ES reais em
    research/docs/sdk-extract/.../MSM7500_OGLES_qcom_sdk_samples (teapot etc.) —
    úteis como corpus de teste do IglHook quando existir execução.

CONCLUSÃO: o firmware/SDK TINHA info decisiva. A camada 3D é DSP-offload (QDSP5)
sob uma fachada GL ES 1.1 ATI-Imageon; o scanout é MDP overlay. HLE na fronteira
IGL é o caminho; LLE do 3D via QDSP5 é possível mas provável beco. Fim da trilha
gráfica até o MAP_CONTROL destravar execução real.

═══════════════════════════════════════════════════════════════════════════════
14. CAMADA CERTA MODELADA: IglHook (produtor real da Fase 1) — 2026-09-07
═══════════════════════════════════════════════════════════════════════════════
Após a §13 provar que PM4/A2xx é a camada errada, modelei o produtor CORRETO:
hook na fronteira IGL/IEGL (gpIGL/gpIEGL), traduzindo para IGpuRasterizer.

Arquivos novos (tools/cpp/gpu/, build isolado):
  - igl_hook.h / igl_hook.cpp — o produtor. GuestMachine abstrai a máquina ARM
    (implementável por Unicorn no LLE OU por um fake no teste — desacopla do core
    e respeita a revisão concorrente).
  - igl_smoke.cpp — prova por FRAMEBUFFER sem Unicorn (3/3 PASS: clear preto,
    GLfixed->float=320.0, clear vermelho=0xF800).
  - Makefile.gpu alvo `igl_smoke`.

ABI real respeitada (fonte: brew-abi.md + zeebulator gl_hle.h/.cpp vs AEEGL.h):
  1. IGL=80 slots (AR/Rel/QI + 77 gl*); IEGL=28 (AR/Rel/QI + 25 egl*).
  2. ARMADILHA: slots gl*/egl* NÃO recebem `po` em R0 — o macro real IGL_glClear(p,a)
     passa só `a`. Logo R0 = 1º arg real. Só AR/Rel/QI seguem po-em-R0. Modelado.
  3. Ponteiros de array = VAs guest, lidos SÓ no draw via read_component (GLfixed/
     byte/short->float host, semântica GLES1.x). Implementado em assemble().
  4. GLfixed 16.16 -> float (glClearColorx, glLoadMatrixx). Implementado.

Slots IMPLEMENTADOS (confirmados no corpus): glViewport(79), glClearColorx(8),
  glClear(7), glDrawArrays(26), glDrawElements(27), AddRef/Release/QueryInterface.
  eglSwapBuffers -> end_frame()+begin_frame() (present no fb_sink).

HONESTIDADE / pendências:
  - Índices de slot: SÓ 7/8/9/26/27/79 estão confirmados (zeebulator gl_hle.cpp
    607-679). Os demais (glVertexPointer, glColorPointer, glTexCoordPointer,
    glBindTexture, glTexImage2D, glCompressedTexImage2D, glGenTextures,
    glEnableClientState, glLoadMatrixx...) estão como -1 PLACEHOLDER e caem em stub
    honesto (dispatch_igl retorna false, não finge desenho). Fixar lendo AEEGL.h
    (SDK: platform/.../inc) ou o resto do array de gl_hle.cpp.
  - GuestMachine.arg() assume AAPCS R0..R3 + pilha; o glue real (ler regs do Unicorn)
    é do lado do core ARM — NÃO integrado aqui p/ não colidir com o outro agente.
  - Continua bloqueado a montante: sem MAP_CONTROL não há jogo chamando a vtable;
    igl_smoke usa GuestMachine falso de propósito. Quando MAP_CONTROL destravar,
    plugar GuestMachine no Unicorn + resolver os slots via AEEGL.h.

pm4_adreno.h: mantido como estudo (camada errada, §13). IglHook é o produtor vivo.

═══════════════════════════════════════════════════════════════════════════════
15. AUDITORIA DO IglHook — erros achados e corrigidos — 2026-09-07
═══════════════════════════════════════════════════════════════════════════════
Revisão contra fonte primária (zeebulator gl_hle.cpp 597-680, array real de 80
slots verificado vs AEEGL.h genuíno). Achados:

[ERRO-1] 13 slots marcados PLACEHOLDER (-1) "precisa AEEGL.h" — mas o corpus JÁ
  tinha a tabela completa. Pior: meus chutes ANTERIORES estavam quase todos
  errados. Índices REAIS (corrigidos):
    glBindTexture 5 (eu: 4)      glColor4x 12 (13)      glColorPointer 14 (15)
    glCompressedTexImage2D 15(16) glDeleteTextures 20(19) glEnableClientState 29(28)
    glDisableClientState 25(17)  glGenTextures 36(34)   glLoadMatrixx 47(41)
    glNormalPointer 55(52)       glTexImage2D 74(68)    glVertexPointer 78(76)
  Todos os 40 slots implementáveis agora preenchidos com valor real. Confirmados
  intactos: glClear 7, glClearColorx 8, glClearDepthx 9, glDrawArrays 26,
  glDrawElements 27, glViewport 79.
  LIÇÃO: consultar o array inteiro do gl_hle.cpp ANTES de declarar "pendência
  AEEGL.h" — a fonte estava no corpus o tempo todo.

[ERRO-2] gl*Pointer/EnableClientState não estavam implementados (eram -1), logo
  assemble() nunca tinha arrays habilitados e NENHUM draw real funcionava. Agora
  implementados: glVertexPointer/Color/TexCoord (size,type,stride,ptr) e
  glNormalPointer (type,stride,ptr — SEM size, sempre 3); Enable/DisableClientState
  liga/desliga por enum (VERTEX/COLOR/TEXCOORD/NORMAL_ARRAY).

[ERRO-3 / lacuna de pipeline, o mais importante] O teste de draw REAL (glDrawArrays
  via hook) FALHOU na 1ª execução e expôs uma lacuna: o IglHook passa as coords
  CRUAS do array guest; o soft_rasterizer assume NDC [-1,1]. Em GLES1 real o vértice
  passa por modelview*projection (mvp) E pelo viewport antes de virar pixel.
  NENHUMA ponta faz esse transform ainda:
    - IglHook: acumula mvp_ mas NÃO o aplica em assemble() (matriz = identidade).
    - soft_rasterizer: fill_tri faz só x*0.5+0.5 (assume clip-space pronto).
  => TODO Fase 2: ligar o transform fixed-function. glMatrixMode/LoadIdentity/
     LoadMatrixx/MultMatrixx/Frustumx/Orthox/Rotate/Scale/Translate (slots reais
     46-77) alimentam a pilha modelview+projection; assemble() deve multiplicar
     cada vértice por mvp e aplicar viewport. Sem isso, só clear + geometria já em
     clip-space desenham. Teste ajustado p/ NDC comprova o CAMINHO (assemble->draw->
     framebuffer), não o transform.

VERIFICADO POR EXECUÇÃO: igl_smoke 3/3 PASS (draw centro=0xFFFF; GLfixed=-0.80;
  clear vermelho=0xF800). gpu_smoke antigo intacto.

AUDITORIA GPU 2026-09-09 (commit 19782c7) — defaults normativos GLES1 corrigidos:
  - RenderState agora inicializa blend (ONE,ZERO), depth_func LESS (0x0201), alpha_func
    ALWAYS (0x0207) — antes 0 (ZERO/NEVER) → guest que habilita teste/func sem chamá-lo
    renderizava preto. Regressão TDD no gl_quickwins_smoke (QWd default-blend/clear).
  - glClear(0) agora é no-op (removido `mask==0||`); o gpu_smoke usava mask=0 como
    sentinela de clear — corrigido para 0x4100 (COLOR|DEPTH) no próprio teste.
  - Otimização: textura ligada resolvida UMA vez por triângulo (rase o hash lookup
    per-pixel do sample_texture); comentários de barycentric/blend da/frustum.
  - Próximo: transform fixed-function (mvp+viewport) segue a maior lacuna (Fase 2).

PENDÊNCIAS ATUALIZADAS:
  - [ ] Transform fixed-function (mvp+viewport) — a maior lacuna, Fase 2.
  - [ ] Índices IEGL (28 slots) — não extraídos ainda; ler array IEGL do gl_hle.cpp.
  - [ ] Ligar GuestMachine ao Unicorn (lado do core ARM; pós-MAP_CONTROL).
  - [ ] Texturas: tex_image_2d/ATITC já tem interface; falta o slot->decode->upload.
