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

Duas correções no modelo de registradores do harness destravaram o caminho:

1. o modelo **não escrevia** as leituras na memória do guest (VIC e UART escrevem).
   O kernel lia zero no CAPLENGTH, calculava `HC_LENGTH = 0` e portanto acreditava que
   o USBCMD ficava em `0x100` (e não em `0x140`, como no mapa do MSM);
2. o HCRESET (USBCMD bit 1) só era limpo em `0x140`. Como o kernel escrevia em
   `0x100`, a memória devolvia 2 para sempre e o `ehci_reset` ficava ~180 mil leituras
   no handshake até estourar o timeout (medido: `handshake` com `mask=0x2 done=0x0`).

Depois disso, no guest:

```
msm_hsusb: LLE probe: 2 usb_add_hcd (irq=47)
msm_hsusb: Qualcomm On-Chip EHCI Host Controller
msm_hsusb: new USB bus registered, assigned bus number 1
```

e os contadores de execução (env `ZEEBO_USB_LOG=1`) mostram o HCD de fato iniciando:
`ehci_reset=1 handshake=1 ehci_run=1 ehci_hub_ctrl=5 hub_thread=1 msleep=2`.

**O boot completa até a shell com o host controller rodando** (`uname -a`, `echo`
respondem normalmente). O host controller entra por padrão; `ZEEBO_NOUSB=1` desliga.

## Onde exatamente mexer para entregar a IRQ 47 (mapeado no código)

No `tools/cpp/test_linux_boot.cpp` a entrega de IRQ tem **dois pontos**, e os dois
hoje só olham a palavra 0 (`g_vic_pending[0] & g_vic_en[0]`, IRQs 0-31):

1. a entrega no hook de código (dentro do bloco que roda a cada instrução):
   condição `(g_vic_pending[0] & g_vic_en[0]) != 0` mais o laço que escolhe `nr` com
   `g_vic_cursor` limitado a 32 bits; depois ele escreve CPSR modo IRQ / SPSR / LR e
   salta para o vetor `0xffff0018`;
2. `on_vic_read`, que calcula `act = g_vic_pending[0] & g_vic_en[0]` e é de onde o
   kernel (`vic_handle_irq`) lê o número da IRQ pendente — com a palavra 0 só, a
   IRQ 47 nunca aparece.

`g_vic_en[2]`/`g_vic_pending[2]` já existem e as escritas (ENSET1/ENCLEAR1/CLEAR1)
já são tratadas: falta só a seleção/entrega considerar a palavra 1 e devolver
`32 + bit` no lugar de `bit`.

## Próximo passo (para enumerar o teclado HID)

O host controller já sobe e o boot completa; falta o **dispositivo**:

1. emular o teclado HID na porta 1: hoje `PORTSC` responde `0x1000` (só PP, `CCS=0`,
   ou seja "nada conectado"). Precisa reportar `CCS=1` e atender o reset/enable da
   porta em `ehci_hub_control` (que já roda, `ehci_hub_ctrl=5` no último run);
2. completar as transferências: ler os `qTD`/`qH` da RAM do guest, escrever de volta
   status/bytes transferidos e levantar `USBSTS.USBINT`. O observador
   `ZEEBO_USB_ASYNC=1` já caminha a lista e imprime endpoint/PID/bytes/buffer + os
   primeiros bytes do setup packet. **Nesta etapa a IRQ 47 passa a ser necessária** —
   ver a seção acima com os dois pontos do VIC que hoje só olham a palavra 0;
3. com os descritores respondidos (device → set address → config → HID report) e o
   interrupt IN funcionando, o `usbhid` faz o bind e o guest lista o dispositivo em
   `/sys/bus/usb/devices` — que é o critério do objetivo.
