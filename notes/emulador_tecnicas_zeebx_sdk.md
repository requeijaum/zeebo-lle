# Técnicas de outros emuladores (zeebx / Infuse / SDK) — aplicáveis ao LLE

**Data:** 2026-09-09 | **Fonte:** ~/projects/zeebx-emu/ (docs/), SDK BREW local, openzeobo repo list

---

## 1. Infuse (Tuxality) — o ÚNICO emulador Zeebo funcional

- Repo: github.com/Tuxality/Infuse (vazio; código não público). Blog: tuxality.net/projects/infuse_zeebo_emulator
- **Arquitetura: HLE TOTAL** — reimplementa o subsistema BREW do zero (clean-room RE), sem BIOS, sem NAND.
- CPU: **dynarmic** (JIT ARM) | GPU: OpenGL ES 1.x | Áudio: MIDI/PCM/ADPCM/MP3 | Entrada: HID + 2 gamepads
- **Jogáveis**: Double Dragon, Crash Nitro Kart 3D, Zeebo Family Pack. Em progresso: Quake II, Reckless Racing, Asphalt, KH V-Cast.
- Ferramenta aberta: github.com/Tuxality/ggzbrewtools (C++17, unpack GGZ).
- **O ponto**: o Infuse PROVA que HLE de BREW funciona — e NINGUÉM boota o REX/L4/AMSS real como o LLE está tentando.

## 2. Zeebx (~/projects/zeebx-emu) — HLE BREW em Rust

- **Contorna L4/REX/AMSS inteiro**: stubbada a API BREW (IShell/IDisplay/...); roda só o .mod do jogo num Unicorn.
- `session.rs`: carrega `ModImage`, `loader::load`, cria Unicorn, entrega `EVT_APP_START`, gira laço de eventos com `INSTRUCTION_BUDGET` (evita laço infinito em slot de API).
- `atc.rs`: texturas ATITC (formato Adreno 130; IMPORTANTE p/ o render do LLE — Boomerang Sports usa só ATITC).
- Fonte: não copiar código (proveniência), usar p/ confirmar ABI/ordem.

## 3. SDK BREW local (~/projects/zeebo-emulator/research/docs/sdk-extract/...)
- BREW MP SDK 7.12.5 Pro: 1668 arquivos .c/.cpp/.h de OEM (headers + código OEM).
- `OEMModTable.c`: tabela estática de módulos/classes BREW que o OEM inclui via FEATURE flags — a ordem de instanciação dos serviços de sistema BREW.
- `OEMSVC.c` (msm): inicialização de serviços no ishellbased — mostra os serviços que esperam subir no boot BREW.
- ATENÇÃO: ig_naming é servidor **REX/L4 custom do Zeebo**, NÃO está no SDK BREW genérico — o SDK mostra o lado BREW (IShell/applets), não o lado L4.

## 4. Outras técnicas úteis
- `zeebo_doom` (tripleoxygen): port PrBoom — "a melhor documentação viva de qual API BREW um jogo real usa".
- `zeeutils` (tripleoxygen): util de config no console.
- `kernel_zeebo` (tripleoxygen): Linux p/ MSM — referência de registradores/periféricos MSM7201A.
- Internet Archive: `zeebo-romset-and-devtools` (~945MB, 68 jogos) — material de teste.

## 5. Trade-off estratégico (relevante AGORA)

O LLE dedica enorme esforço ao boot L4/REX/AMSS (que trava no ig_naming).
**Nenhum emulador funcional (Infuse, Zeebx) passa por essa camada** — todos HLE a BREW.
Isso sugere duas opções p/ o projeto:
- (a) **Continuar LLE do boot** (fiel, mas o ig_naming é um stell de camada REX que ninguém resolveu antes)
- (b) **Usar o corpus NAND só como referência** e fazer o LLE pular o L4/REX (HLE parcial: stubbear ig_naming/IPC e entregar EVT_APP_START como Infuse/Zeebx) — o dynarmic JIT já está pronto p/ isso.

## 6. Conclusão p/ o stall do ig_naming
O problema real pode ser de **ORDEM de inicialização** (quem manda o 1º IPC) — a investigação em `notes/ig_naming_init_order.md` está verificando isso.
Mas a alternativa pragmática (Infuse/Zeebx provam) é **não bootar o REX/L4 de verdade**: stubbar o ig_naming e entregar a mensagem esperada, ou contornar direto para o applet BREW.
