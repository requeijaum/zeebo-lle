# Linux 3.4.113 no MSM7201A — harness, kernel e periféricos

Documenta a linha de trabalho `linux-boot`: bootar um **kernel Linux real** no
emulador LLE do Zeebo, com rootfs limpo e shell funcional, saída no framebuffer e
janela SDL2 (Wayland) para depuração.

Harness: `tools/cpp/test_linux_boot.cpp` (alvo `test_linux_boot` no Makefile).
Kernels versionados em `testdata/kernels/`; patches e fontes do rootfs em
`testdata/kernel-patches/` e `testdata/rootfs/`.

## Como rodar

```bash
cd tools/cpp
# boot padrão: shell no console de VT, host controller USB habilitado
ZEEBO_SDL=1 ZEEBO_BUDGET=380000000 \
  ZEEBO_KERNEL=../../testdata/kernels/zImage-3.4.113-rootfs.bin ./test_linux_boot
```

Sem `ZEEBO_KERNEL` o harness procura `images/zImage`; sem imagem ele sai com
**exit 77 (SKIP)** — é assim que entra em `check-full` sem quebrar em máquina sem
os kernels.

### Variáveis de ambiente (instrumentos)

| variável | efeito |
| :--- | :--- |
| `ZEEBO_KERNEL` | caminho do zImage a bootar |
| `ZEEBO_BUDGET` | teto de instruções emuladas |
| `ZEEBO_SDL` | abre a janela SDL2 (Wayland; renderer software) |
| `ZEEBO_FPS` | teto de redesenho da janela (padrão 60) |
| `ZEEBO_VSYNC` | pede sincronia do compositor no renderer (bloqueia a thread da emulação) |
| `ZEEBO_FB_TEXT` | **decodifica o framebuffer como texto** usando a fonte 8x16 do kernel |
| `ZEEBO_KEY_TEST` | auto-teste determinístico da tabela de teclas (18 casos, roda antes da emulação) |
| `ZEEBO_USB_LOG` | log de registradores USB + contadores rolantes por função do EHCI/hub |
| `ZEEBO_USB_ASYNC` | observador da lista assíncrona do EHCI (QH/qTD, setup packets) |
| `ZEEBO_NOUSB` | tira o host controller USB da cmdline |
| `ZEEBO_PC_CHECK` | lê o texto do kernel na memória do guest p/ validar PC↔símbolo |
| `ZEEBO_VEC_TEST` | prova que o Unicorn não vetoriza exceções do guest |
| `ZEEBO_IRQ_LOG`, `ZEEBO_TIMER_LOG`, `ZEEBO_UART_LOG`, `ZEEBO_MDP_LOG` | logs de periférico |

## Kernel

- **3.4.113** para MSM/ARMv6, compilado num container Debian 12 (`/work/k6/linux-3.4.113`)
  com o toolchain Sourcery G++ 4.5.2 (`arm-none-linux-gnueabi-`).
- Patches próprios (ver `testdata/kernel-patches/`):
  - `board-halibut.c`: registra `msm_panel` (fb0 720x480) e usa o
    `msm_device_hsusb` já existente na lista do board;
  - `msm_fb.c`: ops `cfb_*` + `fb_setcolreg` + `pan_display` imediato
    (`msm_fb-cfb-ops.patch`) para o fbcon desenhar na memória do framebuffer;
  - `ehci-msm.c`: driver renomeado para `msm_hsusb` (casa com o `dev_id` da tabela de
    clocks do 7x00), tolerância à ausência de transceiver e o `usb_add_hcd` que
    faltava, atrás de `zeebo_usb=1` (`usb-ehci-msm.md`).
- Run-time: `ZEEBO_FB_SIZE = 0x00200000` (2MB; 720x480x4 = 1,32MB exige mais que 1MB).

## Rootfs

Initramfs BusyBox 1.36.1 estático com `/dev/console`, `/dev/null`, `/dev/zero`,
`/dev/fb0` (c 29 0), `/dev/tty0` (c 4 0) e `/dev/tty1`. Fontes em `testdata/rootfs/`:

- `init`: monta `/proc` e `/sys`, sobe o `/vtbridge` e roda o shell com
  stdin/stdout no **console de VT (`tty0`)** — é o console que o fbcon desenha, então
  o eco da digitação e as respostas aparecem no painel do framebuffer;
- `vtbridge.c`: lê os bytes que o host injeta na UART (`ttyMSM2`) e os injeta na fila
  de entrada do `tty0` via `TIOCSTI` (compilado estático no container).

## Modelo de periféricos (o que o harness emula)

Os hooks do Unicorn reportam o endereço **físico** dos acessos do guest quando a MMU
está ligada — por isso os periféricos são modelados por PA (e não pelo VA):

| bloco | física | modelo |
| :--- | :--- | :--- |
| VIC | `0xc0000000` | enable/pending de 2 palavras, ack, round-robin na entrega (evita starvation da IRQ 19 do MDP pela 7 do timer) |
| UART1/2/3 | `0xa9a00000` / `0xa9c00000` / `0xa9e00000` | TX/IMR/RX; `ttyMSM2` é o console do kernel (irq 11); teclado do host entra no RX |
| GPT/DGT | VA `0xe0001000` (PA `0xc0100000`) | clockevent virtual: `TIMER_CLEAR` zera o contador, `MATCH`/`ENABLE`; HZ=100 |
| MDP | `0xaa200000` | `INTR_ENABLE/STATUS/CLEAR`, DMA conclui na hora (irq 19) |
| TVENC | `0xaa400000` | mapeado (saída composta do Zeebo) |
| USB HS (EHCI) | `0xa0800000` | capacidade em `0x000` e `0x100` (o driver usa `MSM_USB_BASE+0x100`), CAPLENGTH=0x40, PORTSC com dispositivo conectado/habilitado em high-speed, reset de porta completado na leitura |
| SMEM | PA `0x01f00000` | usado pelo caminho de boot do firmware |

**Framebuffer**: PA `0x15000000`, 720x480 RGB565, `line_length = 1440`, buffer duplo
(yres_virtual = 2*yres) — o harness lê a metade com mais tinta.

## Estado verificado

- Kernel boota até a shell BusyBox com `/proc` montado; keypad do host chega na shell.
- `Console: switching to colour frame buffer device 90x30`; o texto do console de VT
  aparece na memória de framebuffer (conferido com `ZEEBO_FB_TEXT=1`).
- Janela SDL2/Wayland **1x2**: painel esquerdo = UART (`ttyMSM2`), direito =
  framebuffer do guest, os dois sempre desenhados, com limitador de 60 fps e
  releitura do FB só quando o guest escreve nele (~55 fps efetivos medidos).
- Host controller USB: `new USB bus registered, assigned bus number 1` e o hub
  **enumera** o dispositivo (`usb 1-1: new high-speed USB device number 2`).
- Pendente (objetivo): **teclado HID enumerado**. O hub para em
  `device descriptor read/64, error -110` porque falta o motor de transferência —
  executar os qTD da lista assíncrona do EHCI, escrever status/bytes de volta,
  levantar `USBSTS.USBINT` e entregar a IRQ 47 (`INT_USB_HS`; hoje o VIC só entrega
  IRQs 0-31 — os dois pontos a mudar estão mapeados em `testdata/kernel-patches/usb-ehci-msm.md`),
  mais os descritores do HID (device/config/report) e o endpoint de interrupção.

## Lições que custaram tempo

1. **SWI/IRQ/exceções**: o Unicorn não entrega exceções do guest ao vetor do guest;
   o host precisa emular a entrada de exceção ARM1136 (SPSR/LR/modo/vetor) e o FSR
   correto para escrita é `0x807` (o bit 11 é `FSR_WRITE`; `0x407` cai em `do_bad` e
   mata o init).
2. **CAPLENGTH = 0x40** (não 0x20) para o USBCMD cair em `0x140`, como o
   `msm_hsusb_hw.h` define — e o modelo **precisa escrever** as leituras na memória
   do guest, senão o kernel lê zero e deriva o USBCMD errado (0x100).
3. `ASYNCLISTADDR` aponta para o `struct ehci_qh_hw` (bloco DMA só de hardware); a
   `qtd_list` de software fica no `struct ehci_qh`, inalcançável por ali.
4. O `ehci-msm` do 3.4 é um stub: cria o HCD e retorna sem chamar `usb_add_hcd`.

## Branch e próximos passos

**Este trabalho vive na branch `linux-boot`** (decisão de 2026-09-13), separado da
`master`, **até os problemas em aberto estarem resolvidos** — hoje só o teclado HID.
Só depois se discute integrar. A `master` segue como a linha do boot de firmware/BREW.

### Armadilha: VA de símbolo de kernel em instrumento

Os instrumentos que leem estruturas do kernel por endereço fixo (`pseudo_palette`,
a tabela `kDraw[]` de contadores de desenho, `fontdata_8x16`) **apodrecem a cada
rebuild** — o símbolo muda de lugar e o instrumento passa a ler memória qualquer,
*sem falhar*. Isso já custou um bug fantasma: a "flakiness do fbcon" (texto preto
não-determinístico) era o probe da paleta lendo o VA de um kernel anterior e
imprimindo um ponteiro; a paleta sempre esteve correta. O mesmo valia para
`cfb_imageblit=0` com `fbcon_putcs=60`.

Regra: todo VA desses vem com o `grep` do `System.map` que o reconfere, ao lado da
constante. Reconferir depois de **todo** rebuild do kernel:

```sh
grep -E ' (PP|fontdata_8x16|cfb_imageblit|fbcon_putcs|bit_putcs|fbcon_init|fbcon_switch|cfb_fillrect)$' \
  /work/k6/linux-3.4.113/System.map
```

O decodificador de FB tem guard (`fb_font_looks_sane`) que avisa quando o VA da
fonte deixa de fazer sentido, em vez de desenhar lixo silenciosamente.

Depois que o Linux estiver OK, a continuação planejada é a **Fase 17 do ROADMAP:
bootar outros sistemas operacionais** neste mesmo harness (Linux 2.6.29 do período,
Android 1.6/2.x, NetBSD/evbarm, Windows CE/Mobile, e núcleos pequenos como controle
positivo). A ideia é usar cada SO como teste de conformidade independente do modelo de
hardware: caminhos de init diferentes encostam em registradores diferentes, e foi
exatamente assim que esta fase achou a CPU errada, o FSR de escrita e o CAPLENGTH do
EHCI. Quem for estender o harness deve manter os modelos genéricos (do lado do
*hardware*), sem condicionais específicas de um SO.
