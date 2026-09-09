# Pesquisa TripleOxygen / openzeebo — EFS, NAND, MMU e ferramentas
**Data:** 2026-09-09  |  **Fontes:** https://www.tripleoxygen.net/files/devices/zeebo/ , corpus local `tripleoxygen-wiki` , github.com/tripleoxygen/openzeebo

---
## 1. Sistema de arquivos do Zeebo (EFS tree)
### Árvore
```
/
  ├── lctsys - Contém o flixfile.dat e 61s.dat
  ├── mmc4 - Ponto de montagem da eNAND
  │   ├── lctsys - Logs de testes pelo EMAPPLET
  │   ├── mif - Contém os MIFs de todas as aplicações
  │   ├── mod 
  │   │   ├── 32765 - Contém os BLFs de todas as aplicações
  │   │   ├── 32789 - Contém o DK de seu console
  │   │   ├── brewappmgr - Cache do Appmgr
  │   │   ├── discovery_lite
  │   │   └── ?????? - Pastas cujo nome é um número de 6 algarismos, onde está armazenado cada aplicação.
  │   │       │        Pode ser consultado nas páginas de aplicativos e jogos do Zeebo.
  │   │       └── udata - Contém os dados específicos do jogo (save data)
  │   │
  │   ├── shared
  │   ├── sys - Contém o keyboard.cfg (layout do teclado) e hid_devices.cfg (suporte a joysticks/HID)
  │   │   ├── download - Contém arquivos usados no shop da ZeeboNet (incluindo um DK compatível com o console).
  │   │   └── priv - Contém o prefs.dat com parâmetros de configuração do shop da ZeeboNet
  │   ├── user
  │   └── zeeboiddata - Dados do app Zeeboids
  │
  ├── mmgsdi
  │   └── perso
  └── mod
      └── 274755 - Pasta do Zeebo App/Z-Wheel
          └── assets
              ├── faq - HTMLs para a interface do Zeebo.
              │   ├── en
              │   ├── es
              │   └── pt
              ├── games
              │   ├── ???????? - Pastas cujo nome é um número de 8 algarismos (ID do jogo), onde está armazenado descrição e imagens
              │   │              informativas das aplicações/jogos para o shop da ZeeboNet.
              │   └── ...
              ├── stage_slides
              │   ├── ????? - Pastas cujo nome é um número de 5 algarismos, onde está armazenado os slides a serem apresentados na Z-Wheel.
              │   └── .....
              └── zeebo HTMLs para a interface do Zeebo.
                  ├── en
                  ├── es
                  └── pt
```
### Arquivos especiais
```
* fs:/[[61u.key]]
  * fs:/lctsys/[[flixfile.dat]]
  * fs:/lctsys/[[61s.dat]]
  * fs:/mod/274755/[[tectoy.cfg]]
  * fs:/mod/274755/[[tt_dlqueue.db]]
  * fs:/mod/274755/[[tt_game_info]]
  * fs:/mod/274755/[[tt_prefs.db]]
  * fs:/mod/274755/[[uiconfig.xml]]
```
### Identificação das pastas de aplicações
```
A lista abaixo encontra-se incompleta. Sinta-se livre para acrescentar a identificação das pastas faltantes:

  *263019 - Ultimate Chess 3D
  *274214 - Crash Bandicoot Nitro Kart 3D
  *274259 - Action Hero 3D
  *274754 - Double Dragon
  *274791 - Zeebo App (Updater?)
  *274802 - Quake
  *274803 - FIFA 09
  *274804 - Treino Cerebral
  *276121 - Need For Speed: Carbon - Domine a Cidade
  *276152 - Ridge Racer
  *276153 - Quake 2
  *276154 - Prey Evil
  *276212 - Pac-Mania
  *276675 - Resident Evil 4
  *276731 - Tekken 2
  *276809 - Zeebo Extreme Rolimã
  *277083 - Bejeweled Twist
  *277229 - Zeebo Family Pack
  *277380 - Galaxy on Fire
  *277455 - Zenonia
  *277495 - Zeebo Channels (Opera Mini)
  *277534 - [[Zeebo Sports Tênis]]
  *278200 - Heavy Weapon
  *278212 - [[Zeebo Sports Vôlei]]
  *278282 - Rally Master Pro
  *278283 - Zeebo Extreme Jetboard
  *278285 - Zeebo Extreme Bóia Cross
  *278738 - [[Zeebo Sports Queimada]]
  *278962 - Peggle
  *278986 - Caveman Ninja 
  *278987 - Spinmaster
  *278988 - Street Hoop
  *279036 - Um Jogo de Ovos
  *279125 - Super Burger Time
  *279126 - Karnov’s Revenge
  *279159 - [[Zeebo Sports Peteca]]
  *279173 - Wizard Fire
  *279200 - Magical Drop 3
  *279233 - Dark Seal
  *279369 - Alien Breaker Deluxe
  *279380 - Zeebo F.C. Foot Camp
  *279382 - Zeeboids
  *279394 - Zeebo Clube
  *279712 - Zuma's Revenge
  *279888 - Bad Dudes vs. Dragon Ninja
  *280173 - All Star Cards
  *280214 - Armageddon Squadron
  *280221 - Iron Sight
  *280238 - Powerboat Challenge
  *280386 - Alice no País das Maravilhas
  *280394 - Reckless Racing
  *280463 - Tork and Krall
  *280602 - Raging Thunder 2
  *280634 - Turma da Mônica em: Vamos Brincar Vol.1
  *280647 - Zeebo F.C. Super League
  *278965 - Toy Raid
```
## 2. Partições NAND
```
<code>
0:MIBIB
---------------------------------------------------------
 Start block: 	0x0
 Size: 		0xA
 Address: 	0x00000000 - 0x00140000
 Pages: 	0x00000000 - 0x0000027F
 Flash: 	0x00FFFFFF

0:QCSBL
---------------------------------------------------------
 Start block: 	0xA
 Size: 		0x2
 Address: 	0x00140000 - 0x00180000
 Pages: 	0x00000280 - 0x000002FF
 Flash: 	0x00FFFFFF

0:OEMSBL1
---------------------------------------------------------
 Start block: 	0xC
 Size: 		0x3
 Address: 	0x00180000 - 0x001E0000
 Pages: 	0x00000300 - 0x000003BF
 Flash: 	0x00FFFFFF

0:OEMSBL2
---------------------------------------------------------
 Start block: 	0xF
 Size: 		0x3
 Address: 	0x001E0000 - 0x00240000
 Pages: 	0x000003C0 - 0x0000047F
 Flash: 	0x00FFFFFF

0:AMSS
---------------------------------------------------------
 Start block: 	0x12
 Size: 		0xA5
 Address: 	0x00240000 - 0x016E0000
 Pages: 	0x00000480 - 0x00002DBF
 Flash: 	0x00FFFFFF

0:APPSBL
---------------------------------------------------------
 Start block: 	0xB7
 Size: 		0x3
 Address: 	0x016E0000 - 0x01740000
 Pages: 	0x00002DC0 - 0x00002E7F
 Flash: 	0x00FFFFFF

0:FOTA
---------------------------------------------------------
 Start block: 	0xBA
 Size: 		0x2
 Address: 	0x01740000 - 0x01780000
 Pages: 	0x00002E80 - 0x00002EFF
 Flash: 	0x00FFFFFF

0:EFS2
---------------------------------------------------------
 Start block: 	0xBC
 Size: 		0x2A
 Address: 	0x01780000 - 0x01CC0000
 Pages: 	0x00002F00 - 0x0000397F
 Flash: 	0x00FFFFFF

0:APPS
---------------------------------------------------------
 Start block: 	0xE6
 Size: 		0xA9
 Address: 	0x01CC0000 - 0x031E0000
 Pages: 	0x00003980 - 0x000063BF
 Flash: 	0x00FFFFFF

0:FTL
---------------------------------------------------------
 Start block: 	0x18F
 Size: 		0x2
 Address: 	0x031E0000 - 0x03220000
 Pages: 	0x000063C0 - 0x0000643F
 Flash: 	0x00FFFF01

0:EFS2APPS
---------------------------------------------------------
 Start block: 	0x191
 Size: 		0x26F
 Address: 	0x03220000 - 0x07FFFFFF
 Pages: 	0x00006440 - 0x0000FFFF
 Flash: 	0x00FFFFFF
</code>
```
### NAND cheia
```
==== ZeeboMCP ====

Parte dos problemas apresentados pelos consoles são causados pelo esgotamento da NAND, não deixando espaço livre para o sistema trabalhar. Geralmente o espaço livre em um console normal gira entre 30 e 60 MB.

Um componente do sistema chamado de ZeeboMCP fica encarregado de copiar o jogo a ser executado da eNAND para a NAND primeiro. Talvez por questões de desempenho, mas isto causa um delay no início enquanto a cópia é feita. Esta cópia só acontece quando o jogo é executado a partir da Z-Wheel.

Caso após o encerramento do jogo o sistema não consiga mover de volta a cópia da NAND para a eNAND, uma cópia daquele jogo é deixado NAND, o que pode eventualmente esgotá-la, causando instabilidade no sistema.

==== Arquivo de testes ====

O uso da função "NAND speed test" do [[EMAPPLET]] em um console com pouco espaço livre na NAND pode causar problemas, pois o teste deixa um arquivo temporário na memória, ocupando espaço. Zeebos na versão 1.1.0 são mais vulneráveis, já que o arquivo de teste criado pelo EMAPPLET nesta versão é maior.
```
## 3. Mapa de memória física (MSM7201A)
> Fonte: forum.xda RaphaelMemoryMap + `arch/arm/mach-msm/msm_iomap-7x00.h`. Alguns periféricos não confirmados.
```
{| border="1" cellpadding="5" cellspacing="0"
|-
! Endereço físico 
! Descrição
|-
|| 00000000 || SDRAM 32MB (SMI)
|-
|| 01f00000 || SMEM
|-
|| 10000000 || SDRAM 128MB (EBI1)
|-
|| ac000000 || ADSP (AD5) RAM
|-
|| a8000000 || 
|-
|| a0e00000 || MPU
|-
|| a0d00000 || 
|-
|| a0c00000 || 
|-
|| a0a00000 || NAND
|-
|| a0b00000 || mpu
|-
|| a0800000 || HSUSB
|-
|| a0700000 || SDIO3, DEX'd
|-
|| a0600000 || SDIO2, DEX'd
|-
|| a0500000 || SDIO1, DEX'd
|-
|| a0400000 || SDIO0, DEX'd
|-
|| a0200000 || uartDM1
|-
|| a0300000 || uartDM2
|-
|| a0100000 || 
|-
|| a0000000 || HW3D graphics
|-
|| aa700000 || EMDH (MDDI2 host), DEX'd
|-
|| aa600000 || PMDH (MDDI1 host), DEX'd
|-
|| aa500000 || MDC (MDDI client), DEX'd
|-
|| aa400000 || TV_ENC
|-
|| aa300000 || TSSC
|-
|| a8100000 || MSM_SSBI (TS init)
|-
|| a9d00000 || 
|-
|| a9900000 || I2C
|-
|| a8700000 || 
|-
|| aa200000 || MDP
|-
|| a9c00000 || UART3
|-
|| a9b00000 || UART2
|-
|| a9a00000 || UART1
|-
|| a9800000 || USB
|-
|| a9700000 || DEX'd
|-
|| a9600000 || DEX'd
|-
|| a9500000 || DEX'd
|-
|| a9400000 || AMDH00, DEX'd, mARM trusted
|-
|| a9300000 || ARM11_GPIO2 (shadow), DEX'd
|-
|| a9200000 || ARM11_GPIO1 (shadow), DEX'd
|-
|| a9100000 || ARM9_GPIO2, DEX'd
|-
|| a9000000 || ARM9_GPIO1, DEX'd
|-
|| a8600000 || MSM_CLK, DEX'd
|-
|| a8500000 || 
|-
|| a8400000 || 
|-
|| a8300000 || SDRAM_CTL ?
|-
|| a8200000 || 
|-
|| a8200000 || 
|-
|| a8200000 || 
|-
|| a8200000 || 
|-
|| a8200000 || 
|-
|| a8200000 || 
|-
|| a8000000 || 
|-
|| b0000000 || 
|-
|| b0200000 || 
|-
|| b0300000 || 
|-
|| b0400000 || 
|-
|| b0500000 || 
|-
|| b0600000 || 
|-
|| b1000000 || MDSP RAM+registers
|-
|| b8000000 || 
|-
|| b8100000 || 
|-
|| b8200000 || sleep related
|-
|| c0000000 || VIC
|-
|| c0100000 || CSR/GPT, DEX'd
|-
|| c0200000 || 
|}
```
## 4. MMU — mapeamentos de 1º nível (dumps reais)
> VAs não listados têm descritor 00 = inválido/page fault.
### ARM9 (AMSS)
```
=====
<code>
00800000 (VA) = 17b00000 (PA) (SECTION) - [17b00d6a]
00900000 (VA) = 00900000 (PA) (SECTION) - [00900d62]
00b00000 (VA) = 00a87400 (PA) (COARSE ) - [00a87561]
00c00000 (VA) = 00c00000 (PA) (SECTION) - [00c0096a]
00d00000 (VA) = 00d00000 (PA) (SECTION) - [00d0096a]
00e00000 (VA) = 00e00000 (PA) (SECTION) - [00e0096a]
00f00000 (VA) = 00f00000 (PA) (SECTION) - [00f0096a]
01000000 (VA) = 01000000 (PA) (SECTION) - [0100096a]
01100000 (VA) = 01100000 (PA) (SECTION) - [0110096a]
01200000 (VA) = 01200000 (PA) (SECTION) - [0120096a]
01300000 (VA) = 01300000 (PA) (SECTION) - [0130096a]
01400000 (VA) = 00a87c00 (PA) (COARSE ) - [00a87d61]
01500000 (VA) = 01500000 (PA) (SECTION) - [01500d6a]
01600000 (VA) = 01600000 (PA) (SECTION) - [01600d6a]
01700000 (VA) = 01700000 (PA) (SECTION) - [01700d6a]
01800000 (VA) = 01800000 (PA) (SECTION) - [01800d6a]
01900000 (VA) = 01900000 (PA) (SECTION) - [01900d6a]
01a00000 (VA) = 01a00000 (PA) (SECTION) - [01a00d6a]
01b00000 (VA) = 01b00000 (PA) (SECTION) - [01b00d6a]
01c00000 (VA) = 01c00000 (PA) (SECTION) - [01c00d6a]
01d00000 (VA) = 01d00000 (PA) (SECTION) - [01d00d6a]
01e00000 (VA) = 00a88400 (PA) (COARSE ) - [00a88561]
01f00000 (VA) = 01f00000 (PA) (SECTION) - [01f00d66]
16e00000 (VA) = 16e00000 (PA) (SECTION) - [16e0096a]
16f00000 (VA) = 16f00000 (PA) (SECTION) - [16f0096a]
17000000 (VA) = 17000000 (PA) (SECTION) - [1700096a]
17100000 (VA) = 17100000 (PA) (SECTION) - [1710096a]
17200000 (VA) = 17200000 (PA) (SECTION) - [1720096a]
17300000 (VA) = 17300000 (PA) (SECTION) - [1730096a]
17400000 (VA) = 17400000 (PA) (SECTION) - [1740096a]
17500000 (VA) = 00a88c00 (PA) (COARSE ) - [00a88d61]
17600000 (VA) = 17600000 (PA) (SECTION) - [17600d6a]
17700000 (VA) = 17700000 (PA) (SECTION) - [17700d6a]
17800000 (VA) = 17800000 (PA) (SECTION) - [17800d6a]
17900000 (VA) = 17900000 (PA) (SECTION) - [17900d6a]
17a00000 (VA) = 00a89400 (PA) (COARSE ) - [00a89561]
b0000000 (VA) = 00a7d000 (PA) (COARSE ) - [00a7d1e1]
b0100000 (VA) = 00a7dc00 (PA) (COARSE ) - [00a7dde1]
b0200000 (VA) = 00a7e800 (PA) (COARSE ) - [00a7e9e1]
b0300000 (VA) = 00a89c00 (PA) (COARSE ) - [00a89d61]
b0d00000 (VA) = 00a7d400 (PA) (COARSE ) - [00a7d5e1]
b0e00000 (VA) = 00a85c00 (PA) (COARSE ) - [00a85d61]
c0c00000 (VA) = acc00000 (PA) (SECTION) - [acc00d62]
c0f00000 (VA) = a0d00000 (PA) (SECTION) - [a0d00d62]
c1300000 (VA) = a0800000 (PA) (SECTION) - [a0800d62]
c1e00000 (VA) = aa500000 (PA) (SECTION) - [aa500d62]
c2100000 (VA) = a8100000 (PA) (SECTION) - [a8100d62]
c2700000 (VA) = a9b00000 (PA) (SECTION) - [a9b00d62]
c2a00000 (VA) = a9700000 (PA) (SECTION) - [a9700d62]
c2b00000 (VA) = a9600000 (PA) (SECTION) - [a9600d62]
c2c00000 (VA) = a9500000 (PA) (SECTION) - [a9500d62]
c2d00000 (VA) = a9400000 (PA) (SECTION) - [a9400d62]
c3000000 (VA) = a9100000 (PA) (SECTION) - [a9100d62]
c3100000 (VA) = a9000000 (PA) (SECTION) - [a9000d62]
c3200000 (VA) = a8600000 (PA) (SECTION) - [a8600d62]
c3400000 (VA) = a8400000 (PA) (SECTION) - [a8400d62]
c3500000 (VA) = a8300000 (PA) (SECTION) - [a8300d62]
c3600000 (VA) = a8200000 (PA) (SECTION) - [a8200d62]
c3700000 (VA) = a8200000 (PA) (SECTION) - [a8200d62]
c3800000 (VA) = a8200000 (PA) (SECTION) - [a8200d62]
c3900000 (VA) = a8200000 (PA) (SECTION) - [a8200d62]
c3a00000 (VA) = a8200000 (PA) (SECTION) - [a8200d62]
c3b00000 (VA) = a8200000 (PA) (SECTION) - [a8200d62]
c3d00000 (VA) = b0000000 (PA) (SECTION) - [b0000d62]
c4000000 (VA) = b0400000 (PA) (SECTION) - [b0400d62]
c4f00000 (VA) = b1c00000 (PA) (SECTION) - [b1c00d62]
c5000000 (VA) = b8000000 (PA) (SECTION) - [b8000d62]
d1b00000 (VA) = 00a85400 (PA) (COARSE ) - [00a85561]
d1c00000 (VA) = 00a84000 (PA) (COARSE ) - [00a84181]
d1d00000 (VA) = 00a80c00 (PA) (COARSE ) - [00a80da1]
d1e00000 (VA) = 00a7e400 (PA) (COARSE ) - [00a7e5c1]
d1f00000 (VA) = 00a75c00 (PA) (COARSE ) - [00a75de1]
e0000000 (VA) = 00a70800 (PA) (COARSE ) - [00a70801]
e0100000 (VA) = 00a70c00 (PA) (COARSE ) - [00a70c01]
e0200000 (VA) = 00a75000 (PA) (COARSE ) - [00a75001]
e0300000 (VA) = 00a75400 (PA) (COARSE ) - [00a75401]
f0000000 (VA) = 00a00000 (PA) (SECTION) - [00a0040e]
f4000000 (VA) = 00a00000 (PA) (SECTION) - [00a0040a]
f9000000 (VA) = b8000000 (PA) (SECTION) - [b8000c02]
f9300000 (VA) = fff00000 (PA) (SECTION) - [fff00c02]
f9400000 (VA) = 9c000000 (PA) (SECTION) - [9c000c02]
fef00000 (VA) = 00a70400 (PA) (COARSE ) - [00a70401]
ff000000 (VA) = 00a70000 (PA) (COARSE ) - [00a70001]
fff00000 (VA) = 00a75800 (PA) (COARSE ) - [00a75801]
</code>
```
### ARM11 (BREW)
```
<code>
00100000 (VA) = 00100000 (PA) (SECTION ) - [00127c02]
00200000 (VA) = 00200000 (PA) (SECTION ) - [00227c02]
00300000 (VA) = 00300000 (PA) (SECTION ) - [00327c02]
00400000 (VA) = 00400000 (PA) (SECTION ) - [00427c02]
00500000 (VA) = 00500000 (PA) (SECTION ) - [00527c02]
00600000 (VA) = 00600000 (PA) (SECTION ) - [00627c02]
00700000 (VA) = 00700000 (PA) (SECTION ) - [00727c02]
00800000 (VA) = 00800000 (PA) (SECTION ) - [00827c02]
01f00000 (VA) = 01f00000 (PA) (SECTION ) - [01f27c02]
10100000 (VA) = 100ad400 (PA) (COARSE  ) - [100ad401]
10200000 (VA) = 10200000 (PA) (SECTION ) - [1022080e]
10300000 (VA) = 10300000 (PA) (SECTION ) - [1032080e]
10400000 (VA) = 10400000 (PA) (SECTION ) - [1042080e]
10500000 (VA) = 10500000 (PA) (SECTION ) - [1052080e]
10600000 (VA) = 10600000 (PA) (SECTION ) - [1062080e]
10700000 (VA) = 10700000 (PA) (SECTION ) - [1072080e]
10800000 (VA) = 10800000 (PA) (SECTION ) - [1082080e]
10900000 (VA) = 10900000 (PA) (SECTION ) - [1092080e]
10a00000 (VA) = 10a00000 (PA) (SECTION ) - [10a2080e]
10b00000 (VA) = 10b00000 (PA) (SECTION ) - [10b2080e]
10c00000 (VA) = 10c00000 (PA) (SECTION ) - [10c2080e]
10d00000 (VA) = 10d00000 (PA) (SECTION ) - [10d2080e]
10e00000 (VA) = 10e00000 (PA) (SECTION ) - [10e2080e]
10f00000 (VA) = 10f00000 (PA) (SECTION ) - [10f2080e]
11000000 (VA) = 11000000 (PA) (SECTION ) - [1102080e]
11100000 (VA) = 11100000 (PA) (SECTION ) - [1112080e]
11200000 (VA) = 11200000 (PA) (SECTION ) - [1122080e]
11300000 (VA) = 11300000 (PA) (SECTION ) - [1132080e]
11400000 (VA) = 100adc00 (PA) (COARSE  ) - [100adc01]
11500000 (VA) = 11500000 (PA) (SECTION ) - [11520c0e]
11600000 (VA) = 11600000 (PA) (SECTION ) - [11620c0e]
11700000 (VA) = 11700000 (PA) (SECTION ) - [11720c0e]
11800000 (VA) = 11800000 (PA) (SECTION ) - [11820c0e]
11900000 (VA) = 11900000 (PA) (SECTION ) - [11920c0e]
11a00000 (VA) = 11a00000 (PA) (SECTION ) - [11a20c0e]
11b00000 (VA) = 11b00000 (PA) (SECTION ) - [11b20c0e]
11c00000 (VA) = 11c00000 (PA) (SECTION ) - [11c20c0e]
11d00000 (VA) = 11d00000 (PA) (SECTION ) - [11d20c0e]
11e00000 (VA) = 11e00000 (PA) (SECTION ) - [11e20c0e]
11f00000 (VA) = 11f00000 (PA) (SECTION ) - [11f20c0e]
12000000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12100000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12200000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12300000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12400000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12500000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12600000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12700000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12800000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12900000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12a00000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12b00000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12c00000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12d00000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12e00000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
12f00000 (VA) = 12000000 (PA) (SSECTION) - [12060c0e]
13000000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13100000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13200000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13300000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13400000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13500000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13600000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13700000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13800000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13900000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13a00000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13b00000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13c00000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13d00000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13e00000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
13f00000 (VA) = 13000000 (PA) (SSECTION) - [13060c0e]
14000000 (VA) = 14000000 (PA) (SECTION ) - [14020c0e]
14100000 (VA) = 14100000 (PA) (SECTION ) - [14120c0e]
14200000 (VA) = 14200000 (PA) (SECTION ) - [14220c0e]
14300000 (VA) = 14300000 (PA) (SECTION ) - [14320c0e]
14400000 (VA) = 14400000 (PA) (SECTION ) - [14420c0e]
14500000 (VA) = 14500000 (PA) (SECTION ) - [14520c0e]
14600000 (VA) = 14600000 (PA) (SECTION ) - [14620c0e]
14700000 (VA) = 14700000 (PA) (SECTION ) - [14720c0e]
14800000 (VA) = 14800000 (PA) (SECTION ) - [14820c0e]
14900000 (VA) = 100af400 (PA) (COARSE  ) - [100af401]
15400000 (VA) = 100c4400 (PA) (COARSE  ) - [100c4401]
15600000 (VA) = 15600000 (PA) (SECTION ) - [15627c02]
15700000 (VA) = 15700000 (PA) (SECTION ) - [15727c02]
15800000 (VA) = 15800000 (PA) (SECTION ) - [15827c02]
15900000 (VA) = 15900000 (PA) (SECTION ) - [15927c02]
15a00000 (VA) = 15a00000 (PA) (SECTION ) - [15a27c02]
15b00000 (VA) = 15b00000 (PA) (SECTION ) - [15b27c02]
15c00000 (VA) = 15c00000 (PA) (SECTION ) - [15c27c02]
15d00000 (VA) = 15d00000 (PA) (SECTION ) - [15d27c02]
15e00000 (VA) = 15e00000 (PA) (SECTION ) - [15e27c02]
15f00000 (VA) = 15f00000 (PA) (SECTION ) - [15f27c02]
16000000 (VA) = 16000000 (PA) (SECTION ) - [16027c02]
16100000 (VA) = 16100000 (PA) (SECTION ) - [16127c02]
16200000 (VA) = 16200000 (PA) (SECTION ) - [16227c02]
16300000 (VA) = 16300000 (PA) (SECTION ) - [16327c02]
16400000 (VA) = 16400000 (PA) (SECTION ) - [16427c02]
16500000 (VA) = 16500000 (PA) (SECTION ) - [16527c02]
16600000 (VA) = 16600000 (PA) (SECTION ) - [16627c02]
16700000 (VA) = 16700000 (PA) (SECTION ) - [16727c02]
16800000 (VA) = 16800000 (PA) (SECTION ) - [16827c02]
16900000 (VA) = 16900000 (PA) (SECTION ) - [16927c02]
16a00000 (VA) = 16a00000 (PA) (SECTION ) - [16a27c02]
16b00000 (VA) = 16b00000 (PA) (SECTION ) - [16b27c02]
16c00000 (VA) = 16c00000 (PA) (SECTION ) - [16c27c02]
16d00000 (VA) = 16d00000 (PA) (SECTION ) - [16d27c02]
80100000 (VA) = 14b00000 (PA) (SECTION ) - [14b20c0e]
80800000 (VA) = 14c00000 (PA) (SECTION ) - [14c20c0e]
80900000 (VA) = 14d00000 (PA) (SECTION ) - [14d20c0e]
80a00000 (VA) = 14e00000 (PA) (SECTION ) - [14e20c0e]
80b00000 (VA) = 14f00000 (PA) (SECTION ) - [14f20c0e]
80c00000 (VA) = 14a00000 (PA) (SECTION ) - [14a20c0e]
b0100000 (VA) = 100a3800 (PA) (COARSE  ) - [100a3801]
b0400000 (VA) = 100afc00 (PA) (COARSE  ) - [100afc01]
b0d00000 (VA) = 100a3400 (PA) (COARSE  ) - [100a3401]
b0e00000 (VA) = 100a3c00 (PA) (COARSE  ) - [100a3c01]
c0f00000 (VA) = a0d00000 (PA) (SECTION ) - [a0d22c02]
c1300000 (VA) = a0800000 (PA) (SECTION ) - [a0822c02]
c1400000 (VA) = a0700000 (PA) (SECTION ) - [a0722c02]
c1500000 (VA) = a0600000 (PA) (SECTION ) - [a0622c02]
c1600000 (VA) = a0500000 (PA) (SECTION ) - [a0522c02]
c1700000 (VA) = a0400000 (PA) (SECTION ) - [a0422c02]
c1800000 (VA) = a0200000 (PA) (SECTION ) - [a0222c02]
c1d00000 (VA) = aa600000 (PA) (SECTION ) - [aa622c02]
c1e00000 (VA) = aa500000 (PA) (SECTION ) - [aa522c02]
c1f00000 (VA) = aa400000 (PA) (SECTION ) - [aa422c02]
c2300000 (VA) = a9900000 (PA) (SECTION ) - [a9922c02]
c2500000 (VA) = aa200000 (PA) (SECTION ) - [aa222c02]
c2900000 (VA) = a9800000 (PA) (SECTION ) - [a9822c02]
c2a00000 (VA) = a9700000 (PA) (SECTION ) - [a9722c02]
c2e00000 (VA) = a9300000 (PA) (SECTION ) - [a9322c02]
c2f00000 (VA) = a9200000 (PA) (SECTION ) - [a9222c02]
c3200000 (VA) = a8600000 (PA) (SECTION ) - [a8622c02]
c5300000 (VA) = c0000000 (PA) (SECTION ) - [c0022c02]
c5400000 (VA) = c0100000 (PA) (SECTION ) - [c0122c02]
e0000000 (VA) = 10090800 (PA) (COARSE  ) - [10090801]
e0100000 (VA) = 10090c00 (PA) (COARSE  ) - [10090c01]
e0200000 (VA) = 10095000 (PA) (COARSE  ) - [10095001]
e0300000 (VA) = 10095400 (PA) (COARSE  ) - [10095401]
f0000000 (VA) = 10000000 (PA) (SECTION ) - [1000040e]
f0100000 (VA) = 10100000 (PA) (SECTION ) - [1010040e]
f4000000 (VA) = 10000000 (PA) (SECTION ) - [1000040a]
f4100000 (VA) = 10100000 (PA) (SECTION ) - [1010040a]
f9100000 (VA) = c0000000 (PA) (SECTION ) - [c0022c12]
f9200000 (VA) = c0100000 (PA) (SECTION ) - [c0122c12]
f9400000 (VA) = 9c000000 (PA) (SECTION ) - [9c022c12]
fef00000 (VA) = 10090400 (PA) (COARSE  ) - [10090401]
ff000000 (VA) = 10090000 (PA) (COARSE  ) - [10090001]
fff00000 (VA) = 10095800 (PA) (COARSE  ) - [10095801]
</code>
```
## 5. Retirada de firmware / diagnóstico (contexto das ferramentas EFS)
### Porta de diagnóstico
```
====== Porta de diagnóstico ======

O Zeebo oferece uma porta USB Device na parte traseira que pode ser ativada para diagnóstico, transferência de dados e auxílio na depuração de aplicativos rodando no console.

Ela vem desativada de fábrica para evitar que os usuários façam alterações nos arquivos ou extraiam informações internas.

Esta porta - apesar de ser USB 2.0 - apresenta vários problemas e geralmente enumera apenas em 1.1 (full-speed). Controladoras USB mais recentes dificilmente conseguirão enumerar o console, portanto para usar esta porta, possivelmente precisará de um PC mais antigo.

===== Como ativá-la =====
Existem duas maneiras de ativá-la: manualmente pelo AUXSETTINGS (utilitário acessível pelo [[BREW Appmgr]]) ou através da chave [[61u.key]].

==== Pelo AUXSETTINGS ====
Você deve ter acesso ao [[BREW Appmgr]] para acessá-lo. Via [[JTAG]], use o comando "brew".

Do Appmgr, abra o aplicativo "AUXSETTINGS". Nele, navegue em:
 SIO Configuration > Port Map > Diag

Selecione "USB SER1" e confirme. Ele voltará a tela de Port Map. Volte até sair do aplicativo.

==== Pelo 61u.key ====
De posse de seu [[61u.key]], coloque-o na raiz de um cartão SD. Desligue o console e insira o cartão. Ligue o Zeebo e aguarde até aparecer a primeira tela com as instruções do Dragon.

==== Pelo usb.key ====

Caso a versão de seu console seja **[[sistema|1.1.1]]**, crie um arquivo de nome [[usb.key]], vazio, na raiz de um cartão SD. Desligue o console e insira o cartão. Ligue o Zeebo e aguarde até aparecer a primeira tela com as instruções do Dragon.

===== Modos da porta DIAG =====
Sua porta DIAG pode trabalhar de duas formas: Download ou Trace.
  * Download - Apresenta 3 interfaces ao seu PC: Diag, NMEA, Modem.
    * Diag - Porta de diagnóstico, transferência de dados
    * NMEA - Fornece dados NMEA (coordenadas) do GPS (no Zeebo, sem função)
    * Modem - Dá acesso ao modem do console. Talvez ativada no futuro para usar o console como modem.
  * Trace - Apresenta apenas uma interface ao seu PC: Diag.
    * Diag - Mesma funcionalidade acima

A diferença entre elas é que no modo Download, os outros controladores USB são desativados, deste modo, não se pode usar o nenhum periférico (teclado, Dragon, etc) enquanto o Zeebo estiver ligado ao PC. No modo Trace isso não acontece, já que o propósito deste modo é oferecer uma saída de informações dos aplicativos enquanto eles funcionam e assim auxiliar o desenvolvedor.

Estes modos podem ser definidos através do [[EMAPPLET]].

===== Comunicando com o Zeebo =====
Após mapear e ativar a porta por um dos dois métodos acima, basta conectar o Zeebo ao PC. O SO detectará o hardware e solicitará o driver caso ainda não esteja instalado. 

==== Drivers ====
O driver para instalar as portas do Zeebo no PC pode ser obtido através do SDK oficial ou deste //mirror// local: 
  * 32-bit [[https://www.tripleoxygen.net/files/devices/zeebo/driver/Zeebo-Driver-V0.2.zip]]
  * 64-bit [[https://www.tripleoxygen.net/files/devices/zeebo/driver/vista_7_x64.7z]]

==== Aplicações ====
Existem 2 aplicações que podem ser utilizadas para navegar no EFS (sistema de arquivos do BREW) do Zeebo:

  * [[EFS TOOL]] by Yoshihiro [[http://www.4shared.com/file/136359546/96799604/LG_EFS_TOOLS.html | Link]]

  * [[RevSkills]] by bkerler (a versão Free é o suficiente) [[http://psas.revskills.de/?q=node/6 | Link]]
    * [[https://www.tripleoxygen.net/files/devices/zeebo/tool/revskills2.04.zip | Download local]] 



Existem também as ferramentas oficiais da Qualcomm - tal como o QPST - que são opções comerciais, por isso torna-se inviável para nós.

Para usar estas ferramentas, leia os respectivos artigos.
```
### Download mode
```
====== Download_mode ======

Todos dispositivos baseados em SoCs da Qualcomm, oferecem um modo chamado "download", que o prepara para receber um pequeno bootloader que fornecerá recursos de recuperação e manutenção da NAND. O Zeebo, naturalmente, também oferece este modo. Ele é acessível pela [[Porta_de_diagnóstico|porta USB de diagnóstico]] ou pela [[UART|serial]] na placa.

Este modo é implementado pelo QCSBL + OEMSBL, por isso, um firmware corrompido nas regiões de sistema não pode ser recuperado, a não ser por [[JTAG]]. As partições que devem estar íntegras para usar este modo são: PBL (inalterável), QCSBL, OEMSBL e MIBIB.

Para usar esta função no Zeebo, é necessário ter a [[61u.key]] para ativar a porta.

===== Ativação =====
O modo download é ativado em duas ocasiões:
  * Partições corrompidas - ativado automaticamente quando alguma partição está corrompida (aparentemente a maior parte dos casos de consoles danificados onde os LEDs acendem, mas não existe sinal de vídeo)
  * Manualmente - enviar o comando 0x3A pela interface DIAG reinicia o console no modo download.

==== Manualmente ====
Use o [[RevSkills]] ou o [[Bootloader uploader|BLUpload]] na função "-d". Para o RevSkills, primeiro siga até a segunda imagem do respectivo [[RevSkills|artigo]]. **Caso vá enviar bootloaders, use a versão 2.04, a mais recente não funciona corretamente para esta função. Link no final da página.**. Confirmado que a porta está funcional, vá na aba "DIAG" e selecione "3A - DOWNLOAD MODE" na lista dos comandos de diagnóstico.

{{ wiki:rs_dwn_1.png }}

Clique em "Send". Na caixa logo abaixo de aparecer "3A". O console reiniciará, mas não emitirá sinal de vídeo, pois está em modo download e aguarda o bootloader. Ele também reconectará a USB.

Abra a "QC Com Diag Window" novamente e vá para a aba "BL". Caso queira confirmar o modo, na lista de comandos do modo download, selecione "DWNMODE: RequestSoftVer" e clique em "Send Cmd".

{{ wiki:rs_dwn_2.png }}

Se o console entrou no modo download com sucesso, receberá a seguinte versão de software na caixa de baixo:

{{ wiki:rs_dwn_3.png }}

Caso seu console esteja aparentemente danificado (liga, mas sem sinal de vídeo), verifique se ele está em modo download. Provavelmente alguma partição está corrompida e ele entrou neste modo para recuperação.

Neste modo, será possível enviar o bootloader para fazer backup ou flash do firmware.

===== Bootloaders =====
Para enviar um bootloader, marque "Use Intel Hex File" e "Use 7200A hotfix", e desmarque "Enable download mode", pois o console já foi colocado neste modo nos passos acima. Não tente deixar ele fazer tudo sozinho (com esta opção marcada), não funcionará. Faça manualmente. Clique no botão "Run Bootloaderfunctions". O RS solicitará o bootloader a ser enviado. Escolha o arquivo e confirme. Os dados serão enviados e executados em seguida.

Lembre-se de utilizar a versão 2.04 ([[http://www.tripleoxygen.net/files/openzeebo/tool/revskills2.04.zip|download]]) e somente com bootloaders ".hex" aqui. Para usar binários (.bin), utilize o [[Bootloader uploader]].
```
### EMAPPLET
```
====== EMAPPLET ======

O EMAPPLET é um aplicativo de sistema, aparentemente desenvolvido pela Longcheer, que permite fazer algumas alterações no sistema através do [[BREW Appmgr]]. Entre elas, estão informações do sistema, testes de hardware, manutenção da eNAND (formatação, cópia de aplicativos), etc.

===== Menu principal =====

{{ :console:zeebo:emapplet.jpg?400 |}}
===== Acessando o EMAPPLET pela Z-Wheel =====
Na tela principal do Zeebo, o Zeebo App/Z-Wheel, faça a seguinte seqüência no joystick:

  * ZL, Cima (no D-Pad), 3 e Home

Faça rápido, ou no momento que apertar o botão 3, o console tentará atualizar o sistema.

Dentro do EMAPPLET, o botão 2 confirma e 1 volta.

**Nota: Os testes de velocidade da NAND e eNAND no menu de testes de hardware do EMAPPLET, é feito através da criação e leitura de um arquivo em cada uma das respectivas memórias. Estes arquivos ficarão na memória a menos que apague-os manualmente pela [[Porta de diagnóstico]]. Enquanto isso não representa um problema imediato para o Zeebo, percebemos que por algum motivo, um jogo ou app pode ser instalado para a NAND ao invés da eNAND (o correto). Desse modo, caso o jogo copiado seja grande, a soma deste com o arquivo criado pelo EMAPPLET pode causar a exaustão desta memória, causando comportamento errôneo no console.**

===== Field Test =====

A opção Field Test, que contém comandos avançados (incluindo o Memory Copy, que transfere aplicações do cartão SD para a e-NAND interna), só estará acessível com a [[porta de diagnóstico]] **ativa**.
```
### PROC COMM
```
====== Proc_comm ======

Apesar do AMSS fornecer o mecanismo PROC COMM, o BREW aparentemente não utiliza-o, realizando todo o acesso e comunicação com o hardware diretamente (acessando o espaço de memória dos periféricos) ou via RPC.
```
### RevSkills
```
====== RevSkills ======

O RevSkills é um aplicativo escrito por B. Kerler ([[http://psas.revskills.de/|link]]), que permite obter várias informações e realizar diversas rotinas de diagnóstico em hardwares baseados em SoCs da Qualcomm. Uma de suas funções, é o navegador de EFS, que te dará acesso ao sistema de arquivo do Zeebo.

===== Uso - explorando EFS =====
Após instalado (a versão Free é suficiente), execute o programa e acesse a opção "Use Mobile Ports", como mostrado abaixo:

{{ wiki:revskills1b.png }}

Certifique-se que ele detectou seu Zeebo, mostrando em Serial Com Port, a COM que a porta de diagnóstico foi atribuída pela Windows.

{{ wiki:revskills2.png }}

Se estiver em dúvida ou precisa testar se a porta do Zeebo está ativada (via [[JTAG]] ou [[61u.key]]), acesse a aba "DIAG" e clique em "Get HW Details". Observe a saída de informações no campo logo abaixo. Se obter os dados parecidos com o mostrado aqui, está tudo OK. Caso a porta não esteja ativada, o RevSkills irá demorar a responder e virá com dados em branco.

{{ wiki:revskills3.png }}

Confirmado o funcionamento da porta, acesse a aba "EFS". Clique em "Read Directories". Recomendo que remova qualquer cartão SD que esteja plugado no Zeebo antes de ler as pastas, ou o programa tentará enumerar também todos os arquivos do cartão.

{{ wiki:revskills4.png }}

Caso o programa mostre algum erro, tente clicar novamente em "Read Directories". Se persistir, mude o modo de "Use Subsys" para "Use Standard". Às vezes ele pode parar de ler as pastas ou alguns arquivos. Neste caso, desconecte o Zeebo da USB e conecte novamente. É um procedimento um tanto instável.

Você terá a listagem de arquivos em seu Zeebo.

{{ wiki:revskills5.png }}

As pastas com um símbolo de "Pare" não podem ser acessadas. Clique com o botão direito do mouse sobre pastas ou arquivos para acessar as operações de leitura, gravação, etc.

Para entender a estrutura de arquivos, veja [[Sistema de arquivos]].
```
## 6. Bootloader (zloader) — openzeebo
- **Makefile**: `-DARM9` — o zloader é ARM9.
- **start_kernel em 0xc0008848**
- **Periféricos** (notes.txt): NAND MPU `0xa0b00000` (+0x0 enable); Peripheral MPU `0xa0e00000` (+0x400); UART1 GPIO45/46 `0xa9a00000`; clocks `0xa86000e0`; peripheral remap `MCR p15,0,R0,c15,c2,4` = `0x80000016` (ARM1136J-S TRM p.3-164); MPUs aux `0xa8240000`/`0xa8250000`.
- **Bootloader uploader**: `blupload.py`, `crc.py`, `qcserial.py` (Python2 + PySerial); `-d` = modo download, `-f zloader.bin` = envia. Resposta de sucesso: `0@@`.
- **fastboot_protocol.txt**: fastboot padrão Android (host-driven, USB bulk 64B) — não é o boot normal.
- **docs baixados**: `DDI0198E_arm926ejs_r0p5_trm.pdf`, `DDI0211K_arm1136_r1p5_trm.pdf`, `ZeeboDeveloperGuide0.97.pdf` (em `docs/remote/`).

## 7. Implicações para o boot do LLE
1. **Z-Wheel real** vive em `/mod/274755/assets/`: `stage_slides/` (slides 5 dígitos), `games/` (boxart 8 dígitos), `faq/` (HTML).
2. **MIFs** de todas as apps em `/mmc4/mif/`; BLFs em `/mmc4/mod/32765/`; DK em `/mmc4/mod/32789/`; cache do Appmgr em `/mmc4/mod/brewappmgr/`.
3. **Jogos ficam na eNAND** (mmc4); só a Z-Wheel está na NAND (apesar do ZeeboMCP copiar eNAND→NAND antes de executar).
4. **ZeeboMCP**: copia jogo eNAND→NAND antes de executar via Z-Wheel (delay inicial); se não mover de volta, esgota a NAND. Espaço livre normal: 30-60 MB.
5. **Double Dragon** = pasta `274754`; **Zeebo App/Z-Wheel** = `274791` (updater?) e `274755` (app).
6. MMU ARM9 dumps dão os PAs reais p/ validar `0xb0100000` (ig_naming). Região `0x10c...` (ARM11) = AppMgr; `0x01c...` (ARM9) = APPS.
7. Partições NAND dão os offsets exatos p/ a decodificação RLE/descompressão (QW43): APPS = `0x01CC0000`-`0x031E0000`.
8. Retirada de firmware é via download mode (QCSBL+OEMSBL íntegros) + `61u.key` na porta de diagnóstico — hardware real, não emulador.
