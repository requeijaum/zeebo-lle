# Patches de kernel (Zeebo LLE / linux-3.4.113)

Aplicar na arvore `/work/k6/linux-3.4.113` do container `zeebo-xbuild`
(`git apply --directory=...` ou `patch -p1`), depois `make ARCH=arm
CROSS_COMPILE=/work/tc/arm-2011.03/bin/arm-none-linux-gnueabi- zImage`.

## board-halibut-framebuffer.patch — **nao aplicado** no zImage padrao

Registra `msm_device_mdp` + um device `"msm_panel"` (driver em `msm_fb.c`) com
`fb_data` 720x480 e `IORESOURCE_MEM` em PA `0x15000000` (fim da RAM do harness,
fora dos 64MB que o kernel gerencia). `CONFIG_FB_MSM=y` e
`CONFIG_FRAMEBUFFER_CONSOLE=y` ja estao ligados no `.config` do container.

Com o patch aplicado o kernel INSTALA o framebuffer
(`msmfb_probe() installing 720 x 480 panel`), mas o boot para logo depois: o
`fbcon` (e o `fake_vsync` do `msm_fb`, que e' hrtimer) precisa de jiffies, e este
kernel nao tem tick porque o harness ainda nao entrega o clockevent do GPT.

Sem o patch (estado do `testdata/kernels/zImage-3.4.113-rootfs.bin` commitado) o
caminho verificado funciona: `/init` roda, `/proc` monta, shell interativa com
teclado pelo RX da UART e console desenhado na janela SDL2 (Wayland).

Para retomar: fazer o modelo do GPT/DGT satisfazer o kernel (o harness ja tem
`ZEEBO_TIMER=1`, mas hoje o kernel passa a martelar o CSR em `0xC0100000` e nao
sai do inicio do boot), depois reaplicar este patch e blitar o FB
(`0x15000000`, RGB565) na janela no lugar do console.

## Referencias de endereco (MSM7201A, conferidas no fonte)

- VIC: VA `0xE0000000` / PA `0xC0000000` (registradores do `irq.c`).
- GPT/DGT (CSR): VA `0xE0001000` / PA `0xC0100000` (`msm_iomap-7x00.h` e
  `msm_iomap.h`); `INT_GP_TIMER_EXP = 7` (`irqs-7x00.h`).
- UARTs: `0xa9a00000` (UART1), `0xa9c00000` (ttyMSM2, console real), `0xa9e00000`
  (UART3) — identidade VA=PA.
- MDP `0xaa200000` (MSM_MDP_PHYS), TVENC `0xaa400000`.
- Hooks do Unicorn: os de CODIGO reportam VA; os de MEMORIA reportam PA.

## Achado do fbcon x msm_fb (2026-09-12)

Com o patch aplicado o kernel instala o fb0 e o fbcon assume o console
("Console: switching to colour frame buffer device 90x30"), e o kernel escreve na
memoria de FB -- confirmado com hook de escrita: stores em 0x15000000/0x150005a0/
0x15000b40... (clear de uma palavra por linha, passo 1440 = line_length) e o padrao
do `cfb_fillrect`. O TEXTO nao aparece porque `msm_fb.c` guarda o estado do driver em
`fb_info.par` (`struct msmfb_info *msmfb = fb->par`) enquanto o `fbcon` do 3.4 usa o
mesmo campo para o `fbcon_ops` -- conflito de API de framebuffer antiga, nao do
harness. Para o proximo passo: mover o estado do msm_fb para outro lugar (ou portar
tvenc.c/tv_ntsc.c/tv_pal.c como painel de verdade, que e' o que o Zeebo usa).
