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

## Estado verificado

Com `ZEEBO_USB=1` no harness (que acrescenta `zeebo_usb=1` na cmdline):

```
msm_hsusb: LLE probe: 1 entrou (irq=47)
msm_hsusb: no transceiver; continuing without PHY (LLE)
msm_hsusb: LLE probe: 2 usb_add_hcd (irq=47)
msm_hsusb: Qualcomm On-Chip EHCI Host Controller
msm_hsusb: new USB bus registered, assigned bus number 1
```

O `ehci-hcd` fala com o nosso modelo de registradores em `0xa0800000`
(130.265 acessos no run). **Mas o boot trava logo depois**: a enumeração do root
hub submete transferências pelo caminho assíncrono do EHCI (qTD/qH na RAM do
guest) e esse caminho ainda **não** está modelado — o driver fica em polling.

Por isso o bring-up ficou atrás de `zeebo_usb=1`: sem ele o boot padrão segue
limpo (shell + framebuffer + SDL2). Com `ZEEBO_USB=1` o boot vai até
"new USB bus registered" e para aí.

## Próximo passo (para enumerar o teclado HID)

1. modelar a lista assíncrona do EHCI: ler os `qTD`/`qH` da RAM do guest, e
   responder aos control transfers (device descriptor → set address →
   config descriptor → HID report descriptor → interrupt IN);
2. emular um teclado HID na porta 1: `PORTSC` com `CCS=1` + `ehci_hub_control`
   respondendo ao reset/enable da porta;
3. aí o `usbhid` faz o bind e o guest lista o dispositivo em
   `/sys/bus/usb/devices` — que é o critério do objetivo.
