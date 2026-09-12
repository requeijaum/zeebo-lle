# USB/EHCI (host controller) no MSM7x00 — mudanças no kernel 3.4.113

O objetivo é o guest **enumerar um teclado HID**. O que descobrimos, em ordem:

1. Depois de ligar `CONFIG_USB*`/`HID`, o guest já registra `usbcore`, `hub`,
   `ehci_hcd` e `usbhid` — mas `/sys/bus/usb/devices` fica vazio.
2. O platform device `msm_device_hsusb` **já existe** no board do 7x00
   (`board-halibut.c:64`, recursos MEM `0xa0800000` + IRQ 47) e tem os recursos
   certos. Não é preciso criar device novo (criar um segundo dá
   `sysfs: cannot create duplicate filename '/devices/platform/msm_hsusb'`).
3. O driver `drivers/usb/host/ehci-msm.c` deste kernel é um **stub de 250 linhas**:
   `ehci_msm_probe` cria o HCD com `usb_create_hcd`, trata o transceiver e
   **retorna 0 sem nunca chamar `usb_add_hcd`** — por isso não existia host
   controller nenhum.
4. `msm_otg` (OTG/PHY) não existe para o 7x00: a tabela de clocks do 7x00 não tem
   `usb_phy_clk` (só `usb_hs_clk`/`usb_hs_pclk`, com `dev_id="msm_hsusb"`) e
   `clk_get(&pdev->dev, ...)` casa por `(dev_name(dev), con_id)` — com o device
   chamado `msm_hsusb_host` o lookup falha.

## Mudanças aplicadas

`arch/arm/mach-msm/board-halibut.c`

```c
/* USB: usa o msm_device_hsusb ja' registrado pelo board do 7x00. */
```
(um bloco que registrava um device duplicado foi adicionado e **removido**; sobra
só o comentário)

`drivers/usb/host/ehci-msm.c`

```c
/* (1) o nome do driver casa com o dev_id da tabela de clocks do 7x00 */
static struct platform_driver ehci_msm_driver = {
	.probe	= ehci_msm_probe,
	.remove	= __devexit_p(ehci_msm_remove),
	.driver	= {
		.name	= "msm_hsusb",     /* era "msm_hsusb_host" */
		...
```

```c
/* (2) no probe, declara a irq e segue sem transceiver em vez de falhar com -ENODEV */
	unsigned int irq;
	...
	phy = usb_get_transceiver();
	if (!phy) {
		dev_warn(&pdev->dev, "no transceiver; continuing without PHY (LLE)\n");
	} else {
		ret = otg_set_host(phy->otg, &hcd->self);
		...
	}
```

```c
/* (3) registra o HCD no USB core (era o que faltava), atras de zeebo_usb=1 */
	if (!strstr(saved_command_line, "zeebo_usb=1")) {
		dev_info(&pdev->dev, "LLE: zeebo_usb=1 ausente; HCD nao registrado\n");
		return 0;
	}
	irq = platform_get_irq(pdev, 0);
	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
```

## Modelo do EHCI no harness (registradores)

O driver faz `ehci->caps = MSM_USB_BASE + 0x100` (USB_CAPLENGTH) e o core deriva os
operacionais de `caps + CAPLENGTH`; para USBCMD cair em `0x140` — como o
`msm_hsusb_hw.h` define (USBCMD 0x140, USBSTS 0x144, USBINTR 0x148, PORTSC 0x184,
USBMODE 0x1A8) — o **CAPLENGTH tem que ser 0x40**. O `ehci_setup` do core ainda
recalcula `ehci->caps = hcd->regs` (base `0xa0800000`) e lê o capbase ali, então o
modelo responde as capacidades nos **dois** endereços (`0x000` e `0x100`).

Sintoma que isso causava antes: `ehci_reset` ficava **130.261 leituras em polling**
em `0x100` esperando o HCRESET cair (o modelo devolvia lixo ali). Depois da correção
os acessos caíram para **404** e o reset completa.

## Estado verificado

Com `ZEEBO_USB=1` no harness (que acrescenta `zeebo_usb=1` na cmdline):

```
msm_hsusb: LLE probe: 1 entrou (irq=47)
msm_hsusb: no transceiver; continuing without PHY (LLE)
msm_hsusb: LLE probe: 2 usb_add_hcd (irq=47)
msm_hsusb: Qualcomm On-Chip EHCI Host Controller
msm_hsusb: new USB bus registered, assigned bus number 1
```

E, com o modelo de registradores corrigido, o `ehci-hcd` passa do reset (404 acessos
USB no total, nenhum poll infinito). **Mas o boot ainda para logo depois**: o
trabalho seguinte é a enumeração do root hub, que submete transferências pelo
caminho assíncrono do EHCI (qTD/qH na RAM do guest) — e aí o guest está em polling
**sem tocar em registrador USB nenhum**, o que confirma que o que falta é ler/escrever
essas estruturas na RAM, não mais MMIO.

Por isso o bring-up ficou atrás de `zeebo_usb=1`: sem ele o boot padrão segue
limpo (shell + framebuffer + SDL2) — verificado com o mesmo zImage commitado.

## Próximo passo (para enumerar o teclado HID)

1. **entregar a IRQ 47 (INT_USB_HS)**: o modelo de VIC do harness só entrega IRQs
   0-31, mas o USB HS usa a 47. O URB completion do EHCI depende do `ehci_irq`, então
   isso casa com o sintoma observado (o boot para sem tocar em registrador USB
   nenhum, o que é espera, não polling de MMIO). Primeiro passo concreto: estender a
   entrega do VIC para a segunda palavra (IRQs 32-63) e ver se o root hub avança;
2. instrumentar o que o kernel espera: com a 47 entregue, ver se aparecem qTD/qH na
   RAM (o harness já tem o observador `ZEEBO_USB_ASYNC=1`, que caminha a lista
   assíncrona e imprime endpoint/PID/bytes/buffer + os primeiros bytes). Hoje ele
   imprime zero transferências ativas — ou seja, o HCD ainda não chegou a submeter;
3. modelar aí a lista assíncrona: ler os `qTD`/`qH` da RAM do guest, completar as
   transferências (device descriptor → set address → config descriptor → HID report
   descriptor → interrupt IN) e emular o teclado HID na porta 1 (`PORTSC` com
   `CCS=1` + reset/enable da porta). Aí o `usbhid` faz o bind e o guest lista o
   dispositivo em `/sys/bus/usb/devices` — que é o critério do objetivo.

Observador já pronto no harness (env `ZEEBO_USB_ASYNC=1`): caminha a lista
assíncrona, imprime `ep/dev/PID/bytes/buffer` da transferência ativa e os primeiros
16 bytes do buffer — é ele que vai mostrar o setup packet da enumeração quando o
HCD começar a submeter.
